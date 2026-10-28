#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <numeric>
#include "pack/ItemTooltip.h"
#include "pack/BuiltinAssets.h"
#include "pack/GunPackParser.h"
using namespace tacz;
namespace {
pack::GunPackRegistry loadPack() {
    pack::GunPackRegistry packs;
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(std::filesystem::path(TACZ_DEFAULT_PACK_DIR)));
    packs.load(std::move(sources),[](auto const&){},[](auto const&){});
    return packs;
}
auto const& registry() { static auto packs=loadPack(); return packs; }
pack::ItemIdentity ak{pack::ItemKind::Gun,{"tacz","ak47"}};
bool contains(pack::ItemTooltip const& t,std::string_view text) { return t.text().find(text)!=std::string::npos; }
}
TEST_CASE("Embedded generic tooltip translations are exact authorized source values") {
    for (auto locale:{"en_us","zh_cn"}) {
        auto path=std::string("assets/tacz/lang/")+locale+".json";
        auto embedded=pack::builtinAsset(path); REQUIRE(embedded);
        auto selected=pack::parseJsonText(std::string(*embedded));
        std::ifstream input(std::filesystem::path(TACZ_PROJECT_DIR)/"_reference/TACZ/src/main/resources"/path);
        REQUIRE(input.good()); auto source=nlohmann::json::parse(input);
        REQUIRE(selected.size()==10);
        for (auto const& [key,value]:selected.items()) {
            REQUIRE(source.contains(key)); CHECK(value==source.at(key));
            CHECK(registry().localize(key,locale)==value.get<std::string>());
        }
    }
    CHECK(registry().localize("tooltip.tacz.gun.damage","fr_FR")=="Damage: ");
    CHECK(registry().packDisplayName(ak.id,"zh_CN")=="TAC Zero 默认枪械包");
    CHECK(registry().packDisplayName({"missing","x"},"zh_CN").empty());
}
TEST_CASE("Release manifests agree on 0.4.3 and category identity stays language neutral") {
    auto root=std::filesystem::path(TACZ_PROJECT_DIR);
    std::ifstream toothFile(root/"tooth.json"); auto tooth=nlohmann::json::parse(toothFile);
    CHECK(tooth.at("version")=="0.4.3");
    for (auto relative:{"packs/behavior_packs/tacz_bp/manifest.json","packs/resource_packs/tacz_rp/manifest.json"}) {
        std::ifstream file(root/relative); auto manifest=nlohmann::json::parse(file);
        CHECK(manifest.at("header").at("version")==nlohmann::json::array({0,4,3}));
        for (auto const& module:manifest.at("modules")) CHECK(module.at("version")==manifest.at("header").at("version"));
        if (manifest.contains("dependencies")) for (auto const& dependency:manifest.at("dependencies"))
            CHECK(dependency.at("version")==manifest.at("header").at("version"));
    }
    auto tag=pack::itemUserData(ak,&registry().gun(ak.id)->data);
    CHECK_FALSE(tag.contains("display")); CHECK_FALSE(tag.contains("TaczAutoName"));
}
TEST_CASE("Gun titles are white, ammo retains translated formatting, and language uses full code") {
    CHECK(pack::stripFormatting("§e7.62x39 §l测试§r")=="7.62x39 测试");
    CHECK(pack::stripFormatting("中文§")=="中文§");
    CHECK(pack::stripFormatting("§中文§z§E测试")=="§中文§z测试");
    CHECK(pack::itemName(registry(),ak,"zh_CN")=="AKM 突击步枪");
    pack::ItemIdentity ammo{pack::ItemKind::Ammo,{"tacz","762x39"}};
    auto tooltip=pack::buildItemTooltip(registry(),ammo,"zh-CN");
    CHECK(tooltip.lines==std::vector<std::string>{"§r§f§e7.62x39mm 步枪弹§r","§9TAC | 弹药§r","§9§oTAC Zero 默认枪械包§r"});
    CHECK(pack::itemName(registry(),ammo,"en_US")=="7.62x39mm Bullet");
    auto custom=pack::buildItemTooltip(registry(),ak,"zh_CN",{},"My AK");
    CHECK(custom.lines.front()=="§r§fMy AK§r");
}
TEST_CASE("AK47 dynamic tooltip order colours and Java default computations") {
    pack::TooltipState state; state.ammo=30; state.chamber=true; state.mode=pack::FireMode::Auto;
    auto tooltip=pack::buildItemTooltip(registry(),ak,"zh_CN",state);
    REQUIRE(tooltip.ammoIcon);
    CHECK(tooltip.ammoIcon->find("ammo/slot/762x39.png")!=std::string::npos);
    CHECK(tooltip.lines==std::vector<std::string>{
        "§r§fAKM 突击步枪§r",
        "§7从雨林到海湾，永远不会错过它的身影§r",
        std::string(pack::AmmoIconMarker)+"     §6§e7.62x39mm 步枪弹§r",
        "     §831/31§r",
        "§8经验等级：§50 (MAX)§r",
        "§8枪种：§b步枪§r",
        "§8伤害: §b9§r",
        "§625% 原版护甲穿透§r",
        "§6150% 爆头伤害§r",
        "§c移动速度：-5.2%§r",
        "§e§o<<< 在游戏内按下 Z 打开改装界面 >>>§r",
        "§9§oTAC Zero 默认枪械包§r",
        "§9步枪§r"});
    state.ammo=29;
    CHECK(contains(pack::buildItemTooltip(registry(),ak,"zh_CN",state),"30/31"));
    state.ammo=0; state.chamber=false;
    CHECK(contains(pack::buildItemTooltip(registry(),ak,"en_us",state),"0/30"));
    state.dummy=99;
    CHECK(contains(pack::buildItemTooltip(registry(),ak,"en_us",state),"0/30 (99)"));
}
TEST_CASE("Java HideFlags bits independently suppress their own tooltip sections") {
    std::vector<std::string> keys={"从雨林","7.62x39","经验等级","护甲穿透","按下 Z","默认枪械包"};
    for (unsigned bit=0;bit<6;++bit) {
        pack::TooltipState state; state.hideFlags=1u<<bit;
        auto t=pack::buildItemTooltip(registry(),ak,"zh_CN",state);
        for (unsigned other=0;other<6;++other) CHECK(contains(t,keys[other])==(other!=bit));
        CHECK(t.ammoIcon.has_value()==(bit!=1));
        CHECK(t.lines.front()=="§r§fAKM 突击步枪§r");
    }
    pack::TooltipState all; all.hideFlags=63;
    auto t=pack::buildItemTooltip(registry(),ak,"zh_CN",all);
    CHECK(t.lines==std::vector<std::string>{"§r§fAKM 突击步枪§r","§9步枪§r"});
    CHECK(pack::buildItemTooltip(registry(),ak,"zh_CN",pack::TooltipState{.hideFlags=128}).lines.size()==13);
}
TEST_CASE("Tooltip icon marker locates only the remembered complete ammo panel") {
    auto t=pack::buildItemTooltip(registry(),ak,"zh_CN"); auto built=t.text();
    CHECK(pack::tooltipAmmoIconLine(built,built)==2);
    CHECK(pack::tooltipAmmoIconLine("prefix\n"+built,built)==3);
    CHECK_FALSE(pack::tooltipAmmoIconLine("Unrelated item",built));
    CHECK_FALSE(pack::tooltipAmmoIconLine(built,"No marker"));
    CHECK_FALSE(pack::tooltipAmmoIconLine(built+"changed",built+"other"));
}
TEST_CASE("Tooltip uses distance-first damage and mode adjustments like Java, not DPS") {
    // Registry entries are treated read-only in production; this isolated registry
    // lets the pure builder exercise data variants without changing installed packs.
    auto packs=loadPack(); auto gun=const_cast<pack::GunEntry*>(packs.gun(ak.id)); REQUIRE(gun);
    gun->data.bullet.damage=100;
    gun->data.fireModeAdjust[pack::FireMode::Burst]={.damage=2,.armorIgnore=2,.headShotMultiplier=.5};
    gun->data.bolt=pack::Bolt::OpenBolt;
    pack::TooltipState state; state.mode=pack::FireMode::Burst; state.ammo=30; state.chamber=true;
    auto t=pack::buildItemTooltip(packs,ak,"en_us",state);
    CHECK(contains(t,"Damage: §b11")); CHECK(contains(t,"§6100% Minecraft Armor Piercing"));
    CHECK(contains(t,"§6200% Headshot Damage")); CHECK(contains(t,"30/30"));
    gun->data.reload.type=pack::FeedType::Inventory;
    CHECK(contains(pack::buildItemTooltip(packs,ak,"en_us",state),"§eUse Inv. Ammo"));
    auto display=const_cast<pack::GunDisplay*>(packs.gunDisplay(*gun->index.display)); REQUIRE(display);
    gun->data.reload.type=pack::FeedType::Magazine; display->ammoPercent=true;
    state.ammo=15;
    CHECK(contains(pack::buildItemTooltip(packs,ak,"en_us",state),"50%"));
    gun->data.bullet.extraDamage.reset();
    t=pack::buildItemTooltip(packs,ak,"en_us",state);
    CHECK(contains(t,"Damage: §b102")); CHECK(contains(t,"§60% Minecraft Armor Piercing"));
    CHECK(contains(t,"§6100% Headshot Damage"));
}
TEST_CASE("Ammo splitting preserves every requested round at each per-ammo limit") {
    CHECK(pack::splitAmmo(120,60)==std::vector<int>{60,60});
    CHECK(pack::splitAmmo(121,60)==std::vector<int>{60,60,1});
    CHECK(pack::splitAmmo(13,6)==std::vector<int>{6,6,1});
    CHECK(pack::splitAmmo(0,60).empty()); CHECK(pack::splitAmmo(-1,60).empty());
    for (int limit:{1,6,48,60,64,25565}) for (int count=1;count<=2304;++count) {
        auto chunks=pack::splitAmmo(count,limit);
        CHECK(std::accumulate(chunks.begin(),chunks.end(),0)==count);
        CHECK(std::all_of(chunks.begin(),chunks.end(),[&](int n){return n>0 && n<=pack::ammoStackLimit(limit);}));
    }
}
TEST_CASE("Ammo merge ignores only marked auto names and retains all meaningful NBT") {
    nlohmann::json base={{"AmmoId","tacz:762x39"}};
    auto old=base; old["display"]={{"Name","§e7.62x39mm Bullet"}}; old["TaczAutoName"]="§e7.62x39mm Bullet";
    CHECK(pack::canonicalAmmoTag(old)==base);
    old["display"]["Lore"]={"custom lore"};
    CHECK(pack::canonicalAmmoTag(old)!=base);
    old["display"]["Name"]="Renamed";
    CHECK(pack::canonicalAmmoTag(old).at("display").at("Name")=="Renamed");
    auto unmarked=base; unmarked["display"]={{"Name","§e7.62x39mm Bullet"}};
    CHECK(pack::canonicalAmmoTag(unmarked)!=base);
    auto different=base; different["AmmoId"]="tacz:9mm";
    CHECK(pack::canonicalAmmoTag(different)!=base);
    CHECK(pack::mergeAmmoCount(59,10,60,true,true)==1);
    CHECK(pack::mergeAmmoCount(60,10,60,true,true)==0);
    CHECK(pack::mergeAmmoCount(64,10,60,true,true)==0);
    CHECK(pack::mergeAmmoCount(1,10,60,false,true)==0);
    CHECK(pack::mergeAmmoCount(1,10,60,true,false)==0);
}
TEST_CASE("Ammo insertion merges first, permits partial pickup, never overwrites unrelated slots") {
    auto plan=pack::planAmmoInsertion({{0,false},{50,true},{20,false},{60,true},{0,false}},120,60);
    CHECK(plan.additions==std::vector<int>{60,10,0,0,50}); CHECK(plan.remaining==0);
    plan=pack::planAmmoInsertion({{59,true},{60,true},{10,false}},60,60);
    CHECK(plan.additions==std::vector<int>{1,0,0}); CHECK(plan.remaining==59);
    plan=pack::planAmmoInsertion({{64,true},{60,true},{10,false}},60,60);
    CHECK(plan.additions==std::vector<int>{0,0,0}); CHECK(plan.remaining==60);
    CHECK(pack::planAmmoInsertion({},60,60).remaining==60);
    for (int limit:{1,6,48,60,64}) for (int n=0;n<=255;++n) for (int target=0;target<=65;++target) {
        auto slots=std::vector<pack::AmmoSlot>{{target,true},{limit,false},{0,false}};
        auto before=slots;
        auto p=pack::planAmmoInsertion(slots,n,limit);
        CHECK(std::accumulate(p.additions.begin(),p.additions.end(),p.remaining)==n);
        CHECK(p.additions[1]==0);
        CHECK(p.additions[0]<=std::max(0,limit-target));
        CHECK(p.additions[2]<=limit);
        CHECK(slots[0].count==before[0].count); // read-only prediction
    }
}
