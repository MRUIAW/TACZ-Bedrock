#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include "pack/PackItems.h"
#include "pack/GunPackLoader.h"
#include "pack/GunPackParser.h"
#include "model/PackedUV.h"
#include "model/LocalMeshFrame.h"
using namespace tacz;
TEST_CASE("Category item identity and creative-tab NBT keep legacy read semantics") {
    pack::GunData gun; gun.ammoAmount=30; gun.bolt=pack::Bolt::ClosedBolt; gun.fireModes={pack::FireMode::Auto};
    pack::ItemIdentity identity{pack::ItemKind::Gun,{"tacz","ak47"}};
    auto tag=pack::itemUserData(identity,&gun);
    CHECK(pack::itemIdentity(tag)==identity);
    CHECK(tag["GunCurrentAmmoCount"]==30); CHECK(tag["HasBulletInBarrel"]==true); CHECK(tag["GunFireMode"]=="AUTO");
    CHECK(pack::itemType(identity.kind)=="tacz:modern_kinetic_gun");
    // No item-id restriction: a legacy vanilla item carrying GunId remains a gun.
    CHECK(pack::itemIdentity(nlohmann::json{{"GunId","tacz:ak47"}})==identity);
    for (auto kind:{pack::ItemKind::Ammo,pack::ItemKind::Attachment}) {
        auto data=pack::itemUserData({kind,{"tacz","test"}});
        REQUIRE(pack::itemIdentity(data)); CHECK(pack::itemIdentity(data)->kind==kind);
        CHECK(data.size()==1); CHECK_FALSE(data.contains("GunId"));
    }
    CHECK_FALSE(pack::itemIdentity(nlohmann::json{{"AmmoId",42}}));
    CHECK_FALSE(pack::itemIdentity(nlohmann::json{{"AttachmentId","bad id"}}));
    CHECK_THROWS(pack::itemUserData(identity));
    CHECK(pack::ammoStackLimit(60)==60); CHECK(pack::ammoStackLimit(48)==48); CHECK(pack::ammoStackLimit(6)==6);
    CHECK(pack::ammoStackLimit(0)==64); CHECK(pack::ammoStackLimit(25565)==64);
}
TEST_CASE("Locale normalization and en_us fallback are per key, not per language") {
    pack::LanguageTable langs{{"en_us",{{"name","English"},{"fallback","Fallback"}}},{"zh_cn",{{"name","中文"}}}};
    CHECK(pack::lookupTranslation(langs,"name","zh-CN")=="中文");
    CHECK(pack::lookupTranslation(langs,"fallback","zh_CN")=="Fallback");
    CHECK(pack::lookupTranslation(langs,"name","fr_FR")=="English");
    CHECK(pack::lookupTranslation(langs,"missing","zh_CN")=="missing");
    CHECK(pack::normalizeLanguage("tr-TR")=="tr_tr");
}
TEST_CASE("Default item names and slot icons come from gun/ammo display, not atlas guesses") {
    pack::GunPackRegistry packs;
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(std::filesystem::path(TACZ_DEFAULT_PACK_DIR)));
    packs.load(std::move(sources),[](auto const&){},[](auto const&){});
    auto id=*pack::ResourceId::parse("tacz:ak47");
    CHECK(pack::itemName(packs,{pack::ItemKind::Gun,id},"en_US")=="AKM");
    CHECK(pack::itemName(packs,{pack::ItemKind::Gun,id},"does_not_exist")=="AKM");
    for (auto text:{"tacz:ak47","tacz:cz75","tacz:glock_17"}) {
        auto gunId=*pack::ResourceId::parse(text);
        auto const* gun=packs.gun(gunId); REQUIRE(gun);
        CHECK(pack::itemIcon(packs,{pack::ItemKind::Gun,gunId}).has_value());
        REQUIRE(gun->data.ammoId);
        CHECK(pack::itemIcon(packs,{pack::ItemKind::Ammo,*gun->data.ammoId}).has_value());
        auto display=packs.gunDisplay(*gun->index.display); REQUIRE(display);
        auto bytes=packs.readFile(pack::assetPath(*display->model,"geo_models",".json")); REQUIRE(bytes);
        auto geo=model::parseGeoModel(pack::parseJsonText(*bytes));
        for (auto const& bone:geo.bones) for (auto const& q:bone.quads) {
            std::array<model::Vertex,4> v;
            for (int k=0;k<4;++k) v[k]={q.pos[k]/16.f,q.uv[k],q.normal};
            auto safe=model::packedRepeatUVQuads(v);
            CHECK(safe.size()>=4);
            for (auto const& p:safe) { CHECK(p.uv.x>=0); CHECK(p.uv.x<=1); CHECK(p.uv.y>=0); CHECK(p.uv.y<=1); }
        }
    }
    CHECK_FALSE(pack::itemIcon(packs,{pack::ItemKind::Gun,{"missing","missing"}}));
}
TEST_CASE("Registered categories have no vanilla use, durability or weapon behavior") {
    for (auto name:{"modern_kinetic_gun","ammo","attachment"}) {
        std::ifstream input(std::filesystem::path(TACZ_PROJECT_DIR)/"packs/behavior_packs/tacz_bp/items"/(std::string(name)+".json"));
        auto definition=nlohmann::json::parse(input);
        auto const& item=definition.at("minecraft:item");
        CHECK(item.at("description").at("identifier")==std::string("tacz:")+name);
        auto const& components=item.at("components");
        CHECK(components.size()==3);
        CHECK(components.at("minecraft:max_stack_size")== (std::string_view(name)=="ammo" ? 64 : 1));
    }
}
