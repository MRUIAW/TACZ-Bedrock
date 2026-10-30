#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "pack/GunPackLoader.h"
#include "pack/GunPackParser.h"
#include "pack/ItemTooltip.h"
#include "pack/BuiltinAssets.h"
#include "lua/GunLogic.h"
using namespace tacz;
namespace {
class OverrideSource final : public pack::PackSource {
public:
    std::string label;
    std::map<std::string,std::string> files;
    std::string unreadable;
    OverrideSource(std::string name,std::map<std::string,std::string> values):label(std::move(name)),files(std::move(values)) {
        files["gunpack.meta.json"]=R"({"namespace":"fixture"})";
    }
    std::string const& name() const override { return label; }
    std::vector<std::string> list(std::string_view prefix) const override {
        std::vector<std::string> out;
        for (auto const& [p,_]:files) if (p.starts_with(prefix)) out.push_back(p);
        return out;
    }
    bool contains(std::string_view path) const override { return files.contains(std::string(path)); }
    std::optional<std::string> read(std::string_view path) const override {
        auto it=files.find(std::string(path));
        return it==files.end() || path==unreadable ? std::nullopt : std::optional(it->second);
    }
};
auto source(std::string name,std::map<std::string,std::string> files) { return std::make_unique<OverrideSource>(std::move(name),std::move(files)); }
auto load(std::vector<std::unique_ptr<pack::PackSource>> sources) {
    pack::GunPackRegistry out; out.load(std::move(sources),[](auto const&){},[](auto const&){}); return out;
}
}
TEST_CASE("Resource operations preserve delegate order rather than guessing priority from extension") {
    std::string path="assets/fixture/probe/value.json";
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(source("z-first",{{path,"A"},{"assets/fixture/probe/only.json","only"}}));
    sources.push_back(source("a-second",{{path,"B"}}));
    sources.push_back(source("m-third",{{path,"C"}}));
    auto packs=load(std::move(sources));
    CHECK(packs.packs().at(0).name=="z-first");
    CHECK(packs.readFile(path,pack::ResourceLookup::Direct)=="A");
    CHECK(packs.readFile(path,pack::ResourceLookup::Listed)=="C");
    auto listed=packs.resolve(path,pack::ResourceLookup::ListedStack);
    auto stack=packs.resolve(path,pack::ResourceLookup::ResourceStack);
    REQUIRE(listed.size()==3); REQUIRE(stack.size()==3);
    for (size_t i=0;i<3;++i) CHECK(listed[i].bytes==stack[2-i].bytes);
    CHECK(listed[1].pack=="a-second");
    CHECK(packs.readFile("assets/fixture/probe/only.json",pack::ResourceLookup::Direct)=="only");
    CHECK(packs.resolve("assets/fixture/probe/missing.json",pack::ResourceLookup::ListedStack).empty());
    CHECK_FALSE(packs.readFile(path,pack::ResourceLookup::ListedStack));
}
TEST_CASE("An existing namespace does not hide later paths and a failed selected supplier does not fall back") {
    std::string path="assets/fixture/probe/value.bin";
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(source("first",{{"assets/fixture/probe/other.bin","other"}}));
    sources.push_back(source("second",{{path,"content"}}));
    auto broken=source("third",{{path,"unreadable"}}); broken->unreadable=path; sources.push_back(std::move(broken));
    auto packs=load(std::move(sources));
    CHECK(packs.readFile(path,pack::ResourceLookup::Direct)=="content");
    CHECK_FALSE(packs.readFile(path,pack::ResourceLookup::Listed));
    auto selected=packs.resolve(path,pack::ResourceLookup::Listed);
    REQUIRE(selected.size()==1); CHECK(selected.front().pack=="third"); CHECK_FALSE(selected.front().bytes);
}
TEST_CASE("Builtin resources stay beneath packs in direct queries and both kinds of stacks") {
    std::string path="assets/tacz/scripts/default_state_machine.lua";
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(source("one",{{path,"A"}})); sources.push_back(source("two",{{path,"B"}}));
    auto packs=load(std::move(sources));
    CHECK(packs.readFile(path,pack::ResourceLookup::Direct)=="A");
    CHECK(packs.readFile(path,pack::ResourceLookup::Listed)=="B");
    for (auto mode:{pack::ResourceLookup::ListedStack,pack::ResourceLookup::ResourceStack}) {
        auto stack=packs.resolve(path,mode); REQUIRE(stack.size()==3); CHECK(stack.front().builtin);
    }
    packs.clear(); CHECK(packs.readFile(path,pack::ResourceLookup::Direct).has_value());
}
TEST_CASE("Folder discovery preserves native enumeration, not a portable sorted-name assumption") {
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/("tacz-order-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path path; ~Cleanup(){std::error_code ec;fs::remove_all(path,ec);} } cleanup{root};
    fs::create_directories(root);
    for (auto name:{"zeta","Alpha","[middle]"}) {
        fs::create_directory(root/name); std::ofstream(root/name/"gunpack.meta.json")<<R"({"namespace":"fixture"})";
    }
    std::vector<std::string> expected;
    for (auto const& entry:fs::directory_iterator(root)) expected.push_back(entry.path().filename().string());
    pack::GunPackRegistry packs; packs.loadFolder(root,[](auto const&){},[](auto const&){});
    REQUIRE(packs.packs().size()==expected.size());
    for (size_t i=0;i<expected.size();++i) CHECK(packs.packs()[i].name==expected[i]);
}
TEST_CASE("Index and display use listed resources while textures and pack info use direct resources") {
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(source("first",{
        {"data/home/index/ammo/round.json",R"({"name":"home.old","display":"remote:round","stack_size":12})"},
        {"assets/remote/display/ammo/round.json",R"({"slot":"remote:old"})"},
        {"assets/remote/textures/new.png","first pixels"},
        {"assets/home/gunpack_info.json",R"({"name":"home.pack"})"},
        {"assets/home/lang/en_us.json",R"({"home.pack":"Owner pack","itemGroup.tab.tacz.ammo":"Ammo"})"}}));
    sources.push_back(source("second",{
        {"data/home/index/ammo/round.json",R"({"name":"remote.round.name","display":"remote:round","stack_size":5})"},
        {"assets/remote/display/ammo/round.json",R"({"slot":"remote:new"})"},
        {"assets/remote/textures/new.png","second pixels"},
        {"assets/home/gunpack_info.json",R"({"name":"wrong.pack"})"},
        {"assets/remote/gunpack_info.json",R"({"name":"remote.pack"})"},
        {"assets/remote/lang/en_us.json",R"({"remote.round.name":"§6Round","remote.pack":"Other pack"})"}}));
    auto packs=load(std::move(sources));
    pack::ResourceId id{"home","round"};
    REQUIRE(packs.ammo(id)); CHECK(packs.ammo(id)->index.stackSize==5);
    CHECK(packs.ammoSlot(id)==pack::ResourceId{"remote","new"});
    CHECK(packs.readFile("assets/remote/textures/new.png")=="first pixels");
    CHECK(pack::splitAmmo(12,packs.ammo(id)->index.stackSize)==std::vector<int>{5,5,2});
    CHECK(pack::mergeAmmoCount(12,3,5,true,true)==0); // no migration/truncation of old stacks
    auto tooltip=pack::buildItemTooltip(packs,{pack::ItemKind::Ammo,id},"en_us");
    REQUIRE(tooltip.lines.size()==3);
    CHECK(tooltip.lines[0]=="§r§f§6Round§r");
    CHECK(tooltip.lines[2]=="§9§oOwner pack§r"); // item ID namespace, not index/display source
    CHECK(pack::itemIcon(packs,{pack::ItemKind::Ammo,id})=="assets/remote/textures/new.png");
}
TEST_CASE("Language stacks merge keys in Forge order and namespace ownership is independent of pack order") {
    for (bool reversed:{false,true}) {
        std::vector<std::unique_ptr<pack::PackSource>> sources;
        auto owner=source("owner",{{"assets/a/lang/en_us.json",R"({"a.key":"owner","free":"a fallback"})"}});
        auto foreign=source("foreign",{{"assets/z/lang/en_us.json",R"({"a.key":"foreign","free":"z fallback"})"}});
        if (reversed) { sources.push_back(std::move(foreign)); sources.push_back(std::move(owner)); }
        else { sources.push_back(std::move(owner)); sources.push_back(std::move(foreign)); }
        sources.push_back(source("later",{{"assets/a/lang/en_us.json",R"({"a.key":"later owner","a.unique":"unique"})"}}));
        auto packs=load(std::move(sources));
        CHECK(packs.localize("a.key","en_us")=="owner");
        CHECK(packs.localize("a.unique","en_us")=="unique");
        CHECK(packs.localize("free","en_us")=="z fallback");
        REQUIRE(packs.languageOrigin("a.key","fr_fr"));
        CHECK(packs.languageOrigin("a.key","fr_fr")->nameSpace=="a");
        CHECK(packs.languageOrigin("a.key","en_us")->pack=="owner");
    }
}
TEST_CASE("Lua modules are loaded by ID from listed resolution, not by direct file query") {
    std::string path="data/demo/scripts/reload.lua";
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(source("one",{{path,"return { start_reload=function(api) return false end }"}}));
    sources.push_back(source("two",{{path,R"(
        local M={}
        function M.start_reload(api)
            api:cacheScriptData({done=false,empty=api:getReloadStateType()==EMPTY_RELOAD_FEEDING})
            return true
        end
        function M.tick_reload(api)
            local c=api:getCachedScriptData()
            if api:getReloadTime()<100 then
                return c.empty and EMPTY_RELOAD_FEEDING or TACTICAL_RELOAD_FEEDING,100-api:getReloadTime()
            end
            if not c.done then
                local n=api:getNeededAmmoAmount()
                if api:isReloadingNeedConsumeAmmo() then n=api:consumeAmmoFromPlayer(n) end
                api:putAmmoInMagazine(n)
                if c.empty and api:removeAmmoFromMagazine(1)>0 then api:setAmmoInBarrel(true) end
                c.done=true
            end
            return NOT_RELOADING,-1
        end
        return M
    )"}}));
    auto packs=load(std::move(sources));
    CHECK(packs.readFile(path)=="return { start_reload=function(api) return false end }");
    auto data=pack::parseGunData(pack::parseJsonText(R"({"ammo":"demo:round","ammo_amount":10,"bolt":"closed_bolt","script":"demo:reload"})"));
    for (bool empty:{false,true}) for (int stockBefore:{2,20}) for (bool cancel:{false,true}) {
        std::vector<std::string> warnings;
        lua::GunLogic logic(packs,[&](auto const& w){warnings.push_back(w);});
        REQUIRE(logic.loadScript({"demo","reload"}));
        lua::ScriptApi api; api.gunId={"demo","gun"}; api.data=&data; api.maxAmmo=10;
        api.ammo={empty ? 0 : 4,!empty};
        api.reloadState=empty ? gun::ReloadStateType::EmptyFeeding : gun::ReloadStateType::TacticalFeeding;
        int stock=stockBefore;
        api.extractAmmo=[&](int n){int taken=std::min(n,stock);stock-=taken;return taken;};
        REQUIRE(logic.startReload(api) == lua::ReloadStartResult::Started);
        auto before=api.ammo;
        api.reloadTimeMs=0; CHECK(logic.tickReload(api).type==api.reloadState);
        if (cancel) {
            logic.interruptReload(api); logic.clearScriptData(api.shooterKey);
            CHECK(api.ammo==before); CHECK(stock==stockBefore);
        } else {
            api.reloadTimeMs=100; CHECK(logic.tickReload(api).type==gun::ReloadStateType::NotReloading);
            int consumed=std::min(10-before.magazine,stockBefore);
            CHECK(stock==stockBefore-consumed);
            CHECK(api.ammo.magazine==before.magazine+consumed-(empty ? 1 : 0));
            CHECK(api.ammo.barrel);
        }
        CHECK(warnings.empty());
    }
}
TEST_CASE("Language provenance follows the chosen locale before English, including builtin fallback") {
    auto en=pack::parseJsonText(std::string(*pack::builtinAsset("assets/tacz/lang/en_us.json")));
    auto zh=pack::parseJsonText(std::string(*pack::builtinAsset("assets/tacz/lang/zh_cn.json")));
    auto key=en.begin().key(); REQUIRE(zh.contains(key));
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(source("external",{{"assets/foreign/lang/en_us.json",nlohmann::json{{key,"external English"}}.dump()}}));
    auto packs=load(std::move(sources));
    CHECK(packs.localize(key,"zh_cn")==zh.at(key).get<std::string>());
    CHECK_FALSE(packs.languageOrigin(key,"zh_cn"));
    REQUIRE(packs.languageOrigin(key,"fr_fr"));
    CHECK(packs.languageOrigin(key,"fr_fr")->pack=="external");
}
