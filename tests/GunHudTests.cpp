#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include "model/GunHud.h"
#include "model/GunHudLayout.h"
#include "model/GunAlpha.h"
#include "pack/BuiltinAssets.h"
#include "pack/GunPackParser.h"

using namespace tacz;
TEST_CASE("TACZ HUD layout: Java GUI positions, independent font scales and integer fire icon") {
    auto h=model::hud::layout(320,180,18);
    CHECK(h.line==glm::vec4(245,137,246,155));
    CHECK(h.current==glm::vec2(250,137));
    CHECK(h.reserve==glm::vec2(279,137));
    CHECK(h.version==glm::vec2(250,151));
    CHECK(h.gunIcon==glm::vec2(203,136));
    CHECK(h.fireMode==glm::vec2(278,142));
    CHECK(model::hud::Layout::CurrentScale==1.5f);
    CHECK(model::hud::Layout::ReserveScale==.8f);
    CHECK(model::hud::Layout::VersionScale==.5f);
    CHECK(model::hud::Layout::GunSize==glm::vec2(39,13));
    CHECK(model::hud::Layout::ModeSize==glm::vec2(10,10));
    auto percent=model::hud::layout(640,360,24);
    CHECK(percent.reserve==glm::vec2(608,317));
    CHECK(percent.fireMode==glm::vec2(607,322));
    CHECK(percent.current-h.current==glm::vec2(320,180));
}
TEST_CASE("TACZ HUD font mapping: bitmap advance six, no second GUI pixel scale") {
    CHECK(model::hud::bitmapScale(6)==1);
    CHECK(model::hud::bitmapScale(12)==.5f);
    CHECK(model::hud::bitmapScale(0)==1);
    CHECK(model::hud::bitmapScale(std::numeric_limits<float>::quiet_NaN())==1);
    CHECK(model::hud::versionText("1.26.51.01","0.3.21")=="1.26.51-0.3.21");
    CHECK(model::hud::versionText("v1.26.51","0.3.21")=="1.26.51-0.3.21");
    CHECK(model::hud::versionText("1.26.51-LeviLamina","0.3.21")=="1.26.51-0.3.21");
}
TEST_CASE("TACZ HUD colors: strict low ammo, inventory/dummy and overheat priority") {
    CHECK(model::hud::currentColor(7,30,false,false)==0xFF5555);
    CHECK(model::hud::currentColor(8,30,false,false)==0xFFFFFF);
    CHECK(model::hud::currentColor(10,100,true,false)==0xFFFF55);
    CHECK(model::hud::currentColor(10,100,true,true)==0x55FFFF);
    CHECK(model::hud::currentColor(10,100,false,true)==0xFFFFFF);
    CHECK(model::hud::currentColor(100,100,false,false,true)==0xFF5555);
    CHECK(model::hud::currentColor(0,30,true,true)==0xFF5555);
    CHECK(model::hud::reserveColor(false,true)==0x55FFFF);
    CHECK(model::hud::reserveColor(false,false)==0xAAAAAA);
    CHECK(model::hud::reserveColor(true,true)==0xAAAAAA);
}
TEST_CASE("TACZ fresh setgun ammo to HUD: closed 031, open 030; do not invent a chamber") {
    pack::GunData data;
    data.ammoAmount=30;
    for (auto bolt:{pack::Bolt::ClosedBolt,pack::Bolt::ManualAction,pack::Bolt::OpenBolt}) {
        data.bolt=bolt;
        auto initial=gun::initialAmmo(data);
        CHECK(initial.magazine==30); CHECK(initial.barrel);
        auto count=model::hud::currentAmmo(initial.magazine,initial.barrel,bolt);
        CHECK(model::hud::ammoText(count,30,false)==(bolt==pack::Bolt::OpenBolt ? "030" : "031"));
    }
    CHECK(model::hud::currentAmmo(30,false,pack::Bolt::ClosedBolt)==30);
    CHECK(model::hud::currentAmmo(29,true,pack::Bolt::ClosedBolt)==30);
}
TEST_CASE("Gun HUD: TACZ summed chamber count, strict low-ammo limits and reserve") {
    CHECK(model::hud::currentAmmo(30,true,pack::Bolt::ClosedBolt)==31);
    CHECK(model::hud::currentAmmo(30,true,pack::Bolt::ManualAction)==31);
    CHECK(model::hud::currentAmmo(30,true,pack::Bolt::OpenBolt)==30);
    CHECK(model::hud::currentAmmo(0,true,pack::Bolt::OpenBolt)==0);
    CHECK(model::hud::currentAmmo(0,true,pack::Bolt::ClosedBolt)==1);
    CHECK(model::hud::currentAmmo(std::numeric_limits<int>::max(),true,pack::Bolt::ClosedBolt)==9999);
    CHECK(model::hud::lowAmmo(7,30)); CHECK_FALSE(model::hud::lowAmmo(8,30));
    CHECK_FALSE(model::hud::lowAmmo(10,100)); CHECK(model::hud::lowAmmo(9,100));
    CHECK_FALSE(model::hud::lowAmmo(5,20)); CHECK(model::hud::lowAmmo(4,20));
    CHECK(model::hud::ammoText(31,30,false)=="031");
    CHECK(model::hud::reserveText(42,false,false,false)=="0042");
    CHECK(model::hud::reserveText(0,true,false,false)=="9999");
    CHECK(model::hud::reserveText(42,false,true,false)=="\xE2\x88\x9E");
    CHECK(model::hud::reserveText(42,true,true,true).empty());
}
TEST_CASE("Gun HUD: percent style pads three digits and rounds ties even") {
    CHECK(model::hud::ammoText(0,30,true)=="000%");
    CHECK(model::hud::ammoText(3,30,true)=="010%");
    CHECK(model::hud::ammoText(30,30,true)=="100%");
    CHECK(model::hud::ammoText(31,30,true)=="103%");
    CHECK(model::hud::ammoText(1,8,true)=="012%");
    CHECK(model::hud::ammoText(3,8,true)=="038%");
    CHECK(model::hud::ammoText(1,40,true)=="003%"); // actual Java float -> DecimalFormat
    CHECK(model::hud::ammoText(3,40,true)=="008%");
    CHECK(model::hud::ammoText(1,0,true)=="100%");
}
TEST_CASE("Gun HUD: crosshair replaces vanilla, display show_crosshair forces only ADS") {
    model::hud::CrosshairState s;
    CHECK_FALSE(model::hud::crosshairVisible(s)); s.holding=true;
    CHECK(model::hud::crosshairVisible(s)); // show_crosshair=false does NOT hide hip crosshair
    s.aim=.9f; CHECK(model::hud::crosshairVisible(s));
    s.aim=.91f; CHECK_FALSE(model::hud::crosshairVisible(s));
    s.forceInAds=true; CHECK(model::hud::crosshairVisible(s));
    s.animationHide=true; CHECK_FALSE(model::hud::crosshairVisible(s)); s.animationHide=false;
    s.reloading=true; CHECK_FALSE(model::hud::crosshairVisible(s)); s.reloading=false;
    s.puttingAway=true; CHECK_FALSE(model::hud::crosshairVisible(s)); s.puttingAway=false;
    s.firstPerson=false; CHECK_FALSE(model::hud::crosshairVisible(s)); s.firstPerson=true;
    s.hideHud=true; CHECK_FALSE(model::hud::crosshairVisible(s)); s.hideHud=false;
    s.spectator=true; CHECK_FALSE(model::hud::crosshairVisible(s));
    CHECK(model::hud::crosshairType("CROSS_2")=="cross_2");
    CHECK(model::hud::crosshairType("invalid")=="dot_1");
}
TEST_CASE("Hit marker: 300 ms fade, kill-only expansion, head red and reset") {
    model::hud::HitMarker m;
    CHECK(m.sample(100).alpha==0);
    m.mark(100,false,false);
    CHECK(m.sample(100).alpha==1); CHECK(m.sample(250).alpha==doctest::Approx(.5));
    CHECK(m.sample(250).offset==4); CHECK_FALSE(m.sample(250).red);
    CHECK(m.sample(401).alpha==0);
    m.mark(500,true,true);
    CHECK(m.sample(650).alpha==doctest::Approx(.5)); CHECK(m.sample(650).offset==6); CHECK(m.sample(650).red);
    m.mark(660,false,false);
    CHECK(m.sample(670).red); // Java keeps a recent head timestamp until it expires
    CHECK(m.sample(801).offset==4); CHECK_FALSE(m.sample(801).red);
    m={}; CHECK(m.sample(810).alpha==0);
}
TEST_CASE("Gun alpha: Java cutout .1 byte threshold preserves RGB") {
    std::vector<uint8_t> rgba;
    for (int a:{0,1,25,26,104,106,127,128,254,255}) rgba.insert(rgba.end(),{17,33,65,static_cast<uint8_t>(a)});
    auto original=rgba;
    model::javaCutoutMask(rgba);
    for (size_t i=0;i<rgba.size();++i) {
        if (i%4==3) CHECK(rgba[i]==(original[i]>=26 ? 255 : 0));
        else CHECK(rgba[i]==original[i]);
    }
}
TEST_CASE("Gun alpha: immutable group footprint classification, repeat UVs and malformed inputs") {
    std::vector<model::Vertex> v(4);
    v[0].uv={0,0}; v[1].uv={1,0}; v[2].uv={1,1}; v[3].uv={0,1};
    std::vector<uint8_t> rgba(4*4*4,255);
    CHECK(model::groupAlpha(v,rgba,4,4)==model::AlphaClass::Opaque);
    rgba[3]=0; CHECK(model::groupAlpha(v,rgba,4,4)==model::AlphaClass::Cutout);
    rgba[3]=104; CHECK(model::groupAlpha(v,rgba,4,4)==model::AlphaClass::Fractional);
    for (auto& vertex:v) vertex.uv+=glm::vec2(-1,1);
    CHECK(model::groupAlpha(v,rgba,4,4)==model::AlphaClass::Fractional);
    CHECK(model::groupAlpha(v,{},4,4)==model::AlphaClass::Fractional);
    CHECK(model::groupAlpha(std::span(v).first(3),rgba,4,4)==model::AlphaClass::Fractional);
    v[0].uv.x=std::numeric_limits<float>::quiet_NaN();
    CHECK(model::groupAlpha(v,rgba,4,4)==model::AlphaClass::Fractional);
}
TEST_CASE("Gun alpha: classification stays within UV footprint plus conservative filter border") {
    std::vector<model::Vertex> v(4);
    v[0].uv={.375f,.375f}; v[1].uv={.5f,.375f}; v[2].uv={.5f,.5f}; v[3].uv={.375f,.5f};
    std::vector<uint8_t> rgba(8*8*4,255);
    rgba[3]=104; // (0,0) is outside even the filter border
    CHECK(model::groupAlpha(v,rgba,8,8)==model::AlphaClass::Opaque);
    rgba[(2*8+2)*4+3]=0;
    CHECK(model::groupAlpha(v,rgba,8,8)==model::AlphaClass::Cutout);
    rgba[(2*8+2)*4+3]=104;
    CHECK(model::groupAlpha(v,rgba,8,8)==model::AlphaClass::Fractional);
}
TEST_CASE("Gun display: HUD identifiers and ammo count style remain data-driven") {
    auto d=pack::parseGunDisplay(pack::parseJsonText(R"({"hud":"test:gun/full","hud_empty":"test:gun/empty","ammo_count_style":"percent","enable_transparency":true})"));
    REQUIRE(d.hud); REQUIRE(d.hudEmpty); CHECK(d.hud->toString()=="test:gun/full"); CHECK(d.ammoPercent); CHECK(d.enableTransparency);
    auto normal=pack::parseGunDisplay(pack::parseJsonText("{}"));
    CHECK_FALSE(normal.hud); CHECK_FALSE(normal.ammoPercent); CHECK_FALSE(normal.enableTransparency);
}
TEST_CASE("TACZ FaceItem ignores uv_rotation; preserve reference per-face UVs") {
    auto raw=pack::parseJsonText(R"({"format_version":"1.12.0","minecraft:geometry":[{"description":{"texture_width":128,"texture_height":128},"bones":[{"name":"pattern","pivot":[0,0,0],"cubes":[{"origin":[-0.64,8.875,-0.4925],"size":[1.28,1,2.555],"uv":{"west":{"uv":[0,101],"uv_size":[68.25,27.75],"uv_rotation":90}}}]}]}]})");
    auto a=model::parseGeoModel(raw);
    raw["minecraft:geometry"][0]["bones"][0]["cubes"][0]["uv"]["west"].erase("uv_rotation");
    auto b=model::parseGeoModel(raw);
    REQUIRE(a.bones[0].quads.size()==1); REQUIRE(b.bones[0].quads.size()==1);
    CHECK(a.bones[0].quads[0].uv==b.bones[0].quads[0].uv);
    CHECK(a.bones[0].quads[0].uv[2].y==doctest::Approx(128.75/128.0));
    std::array<glm::vec2,4> java{{{.533203125f,.7890625f},{0,.7890625f},{0,1.005859375f},{.533203125f,1.005859375f}}};
    CHECK(a.bones[0].quads[0].uv==java);
    CHECK(a.bones[0].quads[0].uv[0]*256.f==glm::vec2(136.5f,202));
    CHECK(a.bones[0].quads[0].uv[2]*256.f==glm::vec2(0,257.5f));
}
#ifdef TACZ_PROJECT_DIR
TEST_CASE("HUD PNGs are authorized TACZ jar assets and embedded byte-for-byte") {
    auto root=std::filesystem::path(TACZ_PROJECT_DIR);
    auto textures=root/"resources/tacz/textures";
    size_t count=0;
    for (auto const& entry:std::filesystem::recursive_directory_iterator(textures)) if (entry.is_regular_file() && entry.path().extension()==".png") {
        auto relative=std::filesystem::relative(entry.path(),textures).generic_string();
        auto embedded=pack::builtinAsset("assets/tacz/textures/"+relative); REQUIRE(embedded);
        std::ifstream file(entry.path(),std::ios::binary); std::string bytes{std::istreambuf_iterator<char>(file),{}};
        std::ifstream authorized(root/"_reference/TACZ/src/main/resources/assets/tacz/textures"/relative,std::ios::binary);
        std::string reference{std::istreambuf_iterator<char>(authorized),{}};
        CHECK(bytes==reference); CHECK(*embedded==bytes); REQUIRE(embedded->size()>8);
        CHECK(embedded->substr(0,8)==std::string_view("\x89PNG\r\n\x1a\n",8)); ++count;
    }
    CHECK(count==26);
}
#endif
