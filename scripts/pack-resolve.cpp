#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include "pack/GunPackLoader.h"
#include "pack/ItemTooltip.h"
#include "lua/GunLogic.h"
#include "model/Animation.h"
#include "pack/GunPackParser.h"

// Runs the production loader/resolver/VM, never a second precedence model.
// Diagnostic hashes are FNV-1a-64 over the actual returned bytes (not a filename
// hash). This tool is read-only with respect to gun packs and inventories.
namespace {
using namespace tacz;
using json=nlohmann::json;
std::string hash(std::string const& bytes) {
    uint64_t h=14695981039346656037ull;
    for (unsigned char b:bytes) { h^=b; h*=1099511628211ull; }
    std::ostringstream out; out<<std::hex<<std::setfill('0')<<std::setw(16)<<h; return out.str();
}
json describe(std::vector<pack::ResolvedResource> const& values) {
    auto out=json::array();
    for (auto const& r:values) {
        json item={{"pack",r.pack},{"builtin",r.builtin},{"readable",r.bytes.has_value()}};
        if (r.bytes) { item["size"]=r.bytes->size(); item["fnv1a64"]=hash(*r.bytes); }
        out.push_back(std::move(item));
    }
    return out;
}
json consumer(std::string const& path) {
    auto slash=path.find('/',path.find('/')+1);
    if (slash==std::string::npos) return {{"kind","root/non-resource"},{"implemented",false}};
    auto rest=path.substr(slash+1);
    bool asset=path.starts_with("assets/"),data=path.starts_with("data/");
    if (asset && rest.starts_with("lang/")) return {{"kind","language key merge"},{"lookup","ResourceStack"},{"implemented",true}};
    if (asset && rest=="gunpack_info.json") return {{"kind","pack info"},{"lookup","Direct"},{"implemented",true}};
    if (asset && rest.starts_with("tacz_sounds/")) return {{"kind","sound cache"},{"lookup","Listed"},{"implemented",true}};
    if (asset && rest.starts_with("display/")) return {{"kind","display"},{"lookup","Listed"},
        {"implemented",rest.starts_with("display/guns/") || rest.starts_with("display/ammo/")}};
    if ((asset || data) && rest.starts_with("scripts/")) return {{"kind","Lua module"},{"lookup","Listed"},{"implemented",true}};
    if (data && (rest.starts_with("index/") || rest.starts_with("data/"))) return {{"kind","common data/index"},{"lookup","Listed"},
        {"implemented",rest.starts_with("index/guns/") || rest.starts_with("index/ammo/") || rest.starts_with("data/guns/")}};
    if (asset && (rest.starts_with("geo_models/") || rest.starts_with("textures/") || rest.starts_with("animations/")))
        return {{"kind","model/texture/animation"},{"lookup","Direct"},{"implemented",!path.ends_with(".gltf")}};
    if (data && rest.starts_with("recipes/")) return {{"kind","recipe"},{"java_lookup","Listed"},{"implemented",false}};
    if (data && (rest.starts_with("tacz_tags/") || rest.starts_with("tags/") || rest.starts_with("recipe_filters/") || rest.starts_with("tacz_loot_injectors/")))
        return {{"kind","tag/filter/loot merge"},{"java_lookup","ListedStack"},{"implemented",false}};
    return {{"kind","no current consumer"},{"implemented",false}};
}
json reloadProbe(pack::GunPackRegistry const& packs,pack::ResourceId const& script) {
    auto results=json::array();
    for (auto const& [id,entry]:packs.guns()) {
        if (entry.data.script!=script) continue;
        for (std::string scenario:{"tactical","empty","insufficient-tactical","insufficient-empty","cancel-before-feed","cancel-after-feed"}) {
            std::vector<std::string> warnings;
            lua::GunLogic logic(packs,[&](auto const& s){warnings.push_back(s);});
            lua::ScriptApi api; api.gunId=id; api.data=&entry.data; api.maxAmmo=entry.data.ammoAmount;
            bool empty=scenario=="empty" || scenario=="insufficient-empty";
            api.ammo={empty ? 0 : api.maxAmmo/2,!empty};
            api.reloadState=empty ? gun::ReloadStateType::EmptyFeeding : gun::ReloadStateType::TacticalFeeding;
            int stock=scenario.starts_with("insufficient") ? 2 : api.maxAmmo*2;
            int beforeStock=stock; auto before=api.ammo;
            api.extractAmmo=[&](int n){int take=std::min(std::max(0,n),stock);stock-=take;return take;};
            api.inventoryHasAmmo=[&]{return stock>0;};
            bool loaded=logic.loadScript(script),started=loaded && logic.startReload(api),finished=false,cancelled=false;
            if (started) for (int64_t t=0;t<=15000;t+=50) {
                api.reloadTimeMs=t; auto state=logic.tickReload(api); api.reloadState=state.type;
                bool cancel=(scenario=="cancel-before-feed" && t==0) || (scenario=="cancel-after-feed" && api.ammo!=before);
                if (cancel) { logic.interruptReload(api);logic.clearScriptData(api.shooterKey);cancelled=true;break; }
                if (state.type==gun::ReloadStateType::NotReloading) { finished=true;break; }
            }
            results.push_back({{"gun",id.toString()},{"script",script.toString()},{"scenario",scenario},
                {"loaded",loaded},{"started",started},{"finished",finished},{"cancelled",cancelled},{"at_ms",api.reloadTimeMs},
                {"before_magazine",before.magazine},{"after_magazine",api.ammo.magazine},{"barrel",api.ammo.barrel},
                {"stock_before",beforeStock},{"stock_after",stock},{"warnings",warnings}});
        }
    }
    return results;
}
}
int main(int argc,char** argv) try {
    if (argc!=3 && argc!=5) { std::cerr<<"usage: tacz-pack-resolve <gunpacks> <report.json> [--reload-script <id>]\n";return 2; }
    auto root=std::filesystem::u8path(argv[1]);
    if (!std::filesystem::is_directory(root)) throw std::runtime_error("gunpacks directory does not exist");
    pack::GunPackRegistry packs;
    json report={{"hash_algorithm","FNV-1a-64"},{"packs",json::array()},{"messages",json::array()},
        {"conflicts",json::array()},{"ammo",json::object()},{"languages",json::object()}};
    auto log=[&](auto const& s){report["messages"].push_back(s);};
    packs.loadFolder(root,log,log);
    for (auto const& p:packs.packs()) report["packs"].push_back(p.name);
    // Manual bolt metadata only; dispatch is server-side, never a client staging gate.
    report["manual_bolt"] = json::array();
    for (auto const& [id, entry] : packs.guns()) {
        if (entry.data.bolt != pack::Bolt::ManualAction) continue;
        bool clip = false;
        auto display = entry.index.display ? packs.gunDisplay(*entry.index.display) : nullptr;
        if (display && display->animation) if (auto text = packs.readFile(pack::assetPath(*display->animation, "animations", ".animation.json"))) {
            auto clips = animation::parseAnimationFile(pack::parseJsonText(*text));
            clip = clips.contains("bolt");
        }
        report["manual_bolt"].push_back({{"gun", id.toString()}, {"pack", entry.packName},
            {"script", entry.data.script ? json(entry.data.script->toString()) : json(nullptr)},
            {"bolt_clip", clip}, {"inventory_fed", entry.data.reload.type == pack::FeedType::Inventory},
            {"action_seconds", entry.data.boltActionTime}, {"feed_seconds", entry.data.boltFeedTime}});
    }
    for (auto const& path:packs.listFiles("")) {
        auto providers=packs.resolve(path,pack::ResourceLookup::ListedStack);
        size_t external=0; std::set<std::string> contents;
        for (auto const& p:providers) if (!p.builtin) { ++external;if(p.bytes)contents.insert(*p.bytes); }
        if (external<2) continue;
        auto uses=consumer(path);
        json row={{"path",path},{"different_content",contents.size()>1},{"consumer",uses},
            {"Direct",describe(packs.resolve(path,pack::ResourceLookup::Direct))},
            {"Listed",describe(packs.resolve(path,pack::ResourceLookup::Listed))},
            {"ListedStack",describe(providers)},
            {"ResourceStack",describe(packs.resolve(path,pack::ResourceLookup::ResourceStack))}};
        if (uses.value("implemented",false) && uses.contains("lookup")) row["consumer_selection"]=row.at(uses.at("lookup").get<std::string>());
        report["conflicts"].push_back(std::move(row));
    }
    for (auto const& [id,ammo]:packs.ammos()) {
        json row={{"index_pack",ammo.packName},{"stack_limit",pack::ammoStackLimit(ammo.index.stackSize)},
            {"name_key",ammo.index.name},{"display",ammo.index.display ? json(ammo.index.display->toString()) : json(nullptr)}};
        if (auto slot=pack::itemIcon(packs,{pack::ItemKind::Ammo,id})) row["slot"]=*slot;
        for (auto locale:{"en_us","zh_cn"}) row["tooltip"][locale]=pack::buildItemTooltip(packs,{pack::ItemKind::Ammo,id},locale).lines;
        report["ammo"][id.toString()]=std::move(row);
    }
    for (auto const& [locale,values]:packs.languages()) for (auto const& [key,value]:values) {
        json row={{"text",value}};
        if (auto origin=packs.languageOrigin(key,locale)) {
            row["namespace"]=origin->nameSpace;row["pack"]=origin->pack;row["path"]=origin->path;
        } else row["builtin_fallback"]=true;
        report["languages"][locale][key]=std::move(row);
    }
    if (argc==5) {
        if (std::string_view(argv[3])!="--reload-script") throw std::runtime_error("unknown option");
        auto id=pack::ResourceId::parse(argv[4]);if (!id) throw std::runtime_error("invalid script id");
        report["reload_probe"]=reloadProbe(packs,*id);
    }
    std::ofstream output(std::filesystem::u8path(argv[2]),std::ios::binary);
    if (!output) throw std::runtime_error("cannot write report");
    output<<report.dump(2)<<'\n';if (!output) throw std::runtime_error("report write failed");
    std::cout<<"Resolved "<<packs.packs().size()<<" packs, "<<report["conflicts"].size()<<" duplicate paths. Report: "<<argv[2]<<'\n';
} catch (std::exception const& e) { std::cerr<<e.what()<<'\n';return 1; }
