#include <doctest/doctest.h>
#include <filesystem>
#include "gun/ShootRules.h"
#include "lua/AnimationStateMachine.h"
#include "model/GunModelRules.h"
#include "model/MeshDiagnostics.h"
#include "model/RigidGunModel.h"
#include "pack/GunAnimations.h"
#include "pack/GunPackParser.h"

using namespace tacz;
TEST_CASE("Gson 2.10.1 probe: lenient syntax is not global primitive coercion") {
    auto j=pack::parseJsonText("[\"laser\",\n  ]");
    REQUIRE(j.size()==2); CHECK(j[0]=="laser"); CHECK(j[1].is_null());
    CHECK(pack::parseJsonText("[,1,,]")==nlohmann::json::array({nullptr,1,nullptr,nullptr}));
    CHECK(pack::parseJsonText("{unquoted:'value'}")["unquoted"]=="value");
    CHECK(pack::parseJsonText("{x=+1;y:TRUE}")["x"]=="+1");
    CHECK(pack::parseJsonText("{x=+1;y:TRUE}")["y"]==true);
    CHECK(pack::parseJsonText(R"({"base": +0.15})")["base"].is_string());
    auto data=pack::parseGunData(pack::parseJsonText(R"({"movement_speed":{"base":+0.15},"rpm":"600","allow_attachment_types":["laser",]})"));
    CHECK(data.movementSpeed.base==doctest::Approx(.15)); CHECK(data.rpm==600);
    REQUIRE(data.allowAttachmentTypes.size()==2); CHECK_FALSE(data.allowAttachmentTypes[1]);
    for (auto text:{"true","TRUE","false","FALSE","yes"}) {
        auto d=pack::parseGunDisplay(pack::parseJsonText(std::string("{\"show_crosshair\":\"")+text+"\"}"));
        CHECK(d.showCrosshair==(std::string(text)=="true" || std::string(text)=="TRUE"));
    }
    CHECK(pack::parseGunIndex(pack::parseJsonText("{name:42}")).name=="42");
    CHECK_THROWS_AS(pack::parseJsonText("{\"x\":1,}"),pack::ParseError);
    CHECK_THROWS_AS(pack::parseJsonText("[1] [2]"),pack::ParseError);
    CHECK_THROWS_AS(pack::parseGunDisplay(pack::parseJsonText("{show_crosshair:1}")),pack::ParseError);
    CHECK_THROWS_AS(pack::parseGunData(pack::parseJsonText("{rpm:'2.5'}")),pack::ParseError);
    CHECK_THROWS_AS(pack::parseGunData(pack::parseJsonText("{rpm:'2.0'}")),pack::ParseError);
    CHECK_THROWS_AS(pack::parseGunData(pack::parseJsonText("{rpm:' 2 '}")),pack::ParseError);
    CHECK(pack::parseGunData(pack::parseJsonText("{rpm:2.5}")).rpm==2); // JsonTreeReader, not streaming adapter
    CHECK_THROWS_AS(pack::parseGunData(pack::parseJsonText("{rpm:'no'}")),pack::ParseError);
    CHECK_THROWS_AS(pack::parseGunData(pack::parseJsonText("{rpm:'+-1'}")),pack::ParseError);
    CHECK_THROWS_AS(pack::parseGunData(pack::parseJsonText("{bullet:{ignite:{entity:'true'}}}")),pack::ParseError);
    CHECK_THROWS_AS(pack::parseGunData(pack::parseJsonText("{bullet:{extra_damage:{damage_adjust:[{distance:'12',damage:1}]}}}")),pack::ParseError);
    CHECK(pack::parseJsonText("{a:'//not a comment,]', b: /* ok */ +0.15}")["a"]=="//not a comment,]");
}

TEST_CASE("Creative initial raw chamber flag is not an extra round for open bolts") {
    for (auto bolt:{pack::Bolt::OpenBolt,pack::Bolt::ClosedBolt,pack::Bolt::ManualAction}) {
        pack::GunData d; d.bolt=bolt; d.ammoAmount=3;
        auto ammo=gun::initialAmmo(d);
        CHECK(ammo.magazine==3); CHECK(ammo.barrel); // exact TACZ creative builder NBT
        CHECK(gun::hasChamberedRound(bolt,ammo.barrel)==(bolt!=pack::Bolt::OpenBolt));
        if (bolt==pack::Bolt::OpenBolt) {
            for (int i=3;i>0;--i) {
                CHECK(gun::availableRounds(bolt,ammo)==i);
                CHECK(gun::prepareShot(bolt,ammo)==gun::ShotCheck::Ok);
                CHECK(gun::reduceAmmoOnce(bolt,ammo));
            }
            CHECK(gun::availableRounds(bolt,ammo)==0);
            CHECK(gun::prepareShot(bolt,ammo)==gun::ShotCheck::NoAmmo);
            CHECK_FALSE(gun::reduceAmmoOnce(bolt,ammo));
            CHECK_FALSE(gun::hasChamberedRound(bolt,ammo.barrel));
        }
    }
}

TEST_CASE("Asset diagnostics aggregate missing resources once per gun") {
    class Source final : public pack::PackSource {
        std::string label="asset fixture";
        std::map<std::string,std::string> files{
            {"gunpack.meta.json",R"({"namespace":"test"})"},
            {"data/test/index/guns/a.json",R"({"type":"rifle","data":"test:a","display":"test:a"})"},
            {"data/test/data/guns/a.json",R"({"ammo":"test:ammo","bolt":"closed_bolt","fire_mode":["semi"]})"},
            {"assets/test/display/guns/a.json",R"({"model":"test:missing","texture":"test:missing","animation":"test:missing","sounds":{"shoot":"test:missing"}})"}
        };
    public:
        std::string const& name() const override { return label; }
        std::vector<std::string> list(std::string_view prefix) const override {
            std::vector<std::string> out; for (auto const& [path,text]:files) if (path.starts_with(prefix)) out.push_back(path); return out;
        }
        std::optional<std::string> read(std::string_view path) const override {
            auto it=files.find(std::string(path)); return it==files.end() ? std::nullopt : std::optional{it->second};
        }
    };
    std::vector<std::unique_ptr<pack::PackSource>> sources; sources.push_back(std::make_unique<Source>());
    pack::GunPackRegistry packs; std::vector<std::string> lines;
    packs.load(std::move(sources),[&](auto const& line) { lines.push_back(line); },[](auto const&){});
    REQUIRE(packs.guns().size()==1); REQUIRE(packs.assetIssues().size()==1);
    auto const& missing=packs.assetIssues().begin()->second;
    CHECK(std::find_if(missing.begin(),missing.end(),[](auto const& s) { return s.starts_with("model=test:missing"); })!=missing.end());
    CHECK(std::find_if(missing.begin(),missing.end(),[](auto const& s) { return s.starts_with("sound=test:missing"); })!=missing.end());
    CHECK(std::count_if(lines.begin(),lines.end(),[](auto const& s) { return s.starts_with("[gun-assets] test:a missing:"); })==1);
    CHECK_FALSE(packs.hasFile("assets/test/geo_models/missing.json"));
    CHECK(packs.hasFile("assets/tacz/scripts/default_state_machine.lua"));
}

TEST_CASE("Additional magazine instances source geometry but not its collapsed root transform") {
    auto geo=model::parseGeoModel(pack::parseJsonText(R"({"format_version":"1.12.0","minecraft:geometry":[{
      "description":{"texture_width":16,"texture_height":16},"bones":[
      {"name":"root","pivot":[0,0,0]},
      {"name":"lefthand_and_mag","parent":"root","pivot":[0,0,0]},
      {"name":"magazine","parent":"lefthand_and_mag","pivot":[32,0,0],"cubes":[{"origin":[32,0,0],"size":[1,1,1],"uv":[0,0]}]},
      {"name":"mag_standard","parent":"magazine","pivot":[0,0,0],"cubes":[{"origin":[0,0,0],"size":[1,1,1],"uv":[0,0]}]},
      {"name":"additional_magazine","parent":"root","pivot":[0,16,0]}]}]})"));
    auto rigid=model::compileRigidGunModel(geo); model::BoneFrame frame;
    animation::Pose p;
    CHECK_FALSE(model::additionalMagazineKeyed(p));
    p["additional_magazine"]={}; CHECK_FALSE(model::additionalMagazineKeyed(p));
    p["additional_magazine"].translation=glm::vec3(0);
    CHECK(model::additionalMagazineKeyed(p));
    p["magazine"].scale=glm::vec3(0);
    p["lefthand_and_mag"].scale=glm::vec3(0);
    for (bool visible:{false,true}) for (int extent:{0,1}) {
        model::GunRenderState state; state.additionalMagazine=visible; state.extendedMagLevel=extent;
        auto visit=[&](auto const& b,auto* parent) { return model::gunVisibility(b,parent,state); };
        model::evaluateBoneFrame(geo,rigid,p,visit,frame);
        auto old=model::bake(geo,visit,p,true),now=model::flattenRigidGunModel(rigid,frame);
        REQUIRE(old.size()==now.size()); CHECK(model::positionDelta(old,now)<2e-6);
        auto source=geo.find("magazine"),instance=static_cast<int>(geo.bones.size())+source;
        CHECK_FALSE(model::drawableRigidBone(frame,source));
        CHECK(model::drawableRigidBone(frame,instance)==visible);
        CHECK(model::matrixDelta(frame.world[instance],frame.world[geo.find("additional_magazine")])==0);
    }
}

#ifdef TACZ_DEFAULT_PACK_DIR
TEST_CASE("Real affected magazines and pistol put-away scripts use unmodified pack assets") {
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(TACZ_DEFAULT_PACK_DIR));
    pack::GunPackRegistry packs; packs.load(std::move(sources),[](auto const&){},[](auto const&){});
    for (auto name:{"ak47","m4a1","glock_17","hk_mp5a5","deagle_golden","b93r","cz75","deagle","fn_fal","g36k","hk_mk23","m9a4","p320"}) {
        INFO(std::string(name));
        auto entry=packs.gun({"tacz",name}); REQUIRE(entry); REQUIRE(entry->index.display);
        auto display=packs.gunDisplay(*entry->index.display); REQUIRE(display); REQUIRE(display->model);
        auto text=packs.readFile(pack::assetPath(*display->model,"geo_models",".json")); REQUIRE(text);
        auto geo=model::parseGeoModel(pack::parseJsonText(*text));
        auto rigid=model::compileRigidGunModel(geo); model::BoneFrame frame;
        auto clips=pack::loadGunAnimations(packs,*display);
        bool sawAdditional=false;
        for (auto const& [clipName,clip]:clips) {
            if (!clipName.starts_with("reload") && !clipName.starts_with("inspect")) continue;
            for (int i=0;i<=12;++i) {
                animation::Pose pose; animation::evaluate(clip,clip.duration*i/12.f,pose);
                model::GunRenderState state; state.additionalMagazine=model::additionalMagazineKeyed(pose);
                sawAdditional|=state.additionalMagazine;
                auto visit=[&](auto const& b,auto* parent) { return model::gunVisibility(b,parent,state); };
                model::evaluateBoneFrame(geo,rigid,pose,visit,frame);
                auto old=model::bake(geo,visit,pose,true),now=model::flattenRigidGunModel(rigid,frame);
                REQUIRE(old.size()==now.size()); CHECK(model::positionDelta(old,now)<2e-6f);
                bool uv=true; for (size_t k=0;k<old.size();++k) uv&=old[k].uv==now[k].uv; CHECK(uv);
            }
        }
        // B93R's shipped reload clips do not key additional_magazine: its normal
        // magazine follows mag_and_lefthand out of view. Do not fabricate channels.
        if (std::string(name)!="ak47" && std::string(name)!="b93r") CHECK(sawAdditional);
        CHECK_FALSE(rigid.magazineTraversal.empty());
        if (std::string(name)=="glock_17" || std::string(name)=="deagle") {
            REQUIRE(clips.contains("put_away"));
            lua::AnimationContext c; c.ammo=entry->data.ammoAmount; c.barrel=true; c.putAwayTime=entry->data.putAwayTime;
            lua::AnimationStateMachine m(packs,clips,*display->stateMachine,c);
            REQUIRE(m.initialize(0)); REQUIRE(m.trigger("draw")); m.update(1);
            m.setTime(1); REQUIRE(m.trigger("put_away")); CHECK(m.healthy());
            auto t=m.controller().getTrack(0,9); CHECK(m.controller().info(t).name=="put_away");
            CHECK(m.controller().info(t).progress==doctest::Approx(clips.at("put_away").duration-c.putAwayTime));
            m.exit(); m.update(1.1); CHECK(m.healthy()); CHECK(m.controller().info(t).name=="put_away");
        }
    }
}
#endif
