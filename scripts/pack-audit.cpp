#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <array>
#include <cmath>
#include "model/GunModelRules.h"
#include "pack/GunPackLoader.h"
#include "pack/GunPackParser.h"
#include "pack/GunAnimations.h"
#include "model/GeoModel.h"
#include "model/RigidGunModel.h"
#include "gun/ShootRules.h"

// Production parser/registry/model compiler, not a second approximate JSON parser.
// Writes metadata only to the caller's output path; never modifies a gun pack.
int main(int argc,char** argv) {
    if (argc!=3) { std::cerr<<"usage: tacz-pack-audit <gunpacks-directory> <metadata.json>\n"; return 2; }
    using namespace tacz;
    namespace fs=std::filesystem;
    using json=nlohmann::json;
    if (!fs::is_directory(fs::path(argv[1]))) { std::cerr<<"gunpacks directory does not exist\n"; return 2; }
    pack::GunPackRegistry packs;
    json report={{"messages",json::array()},{"guns",json::array()},{"packs",json::array()}};
    auto log=[&](auto const& text) { report["messages"].push_back(text); };
    packs.loadFolder(fs::path(argv[1]),log,log);
    for (auto const& p:packs.packs()) report["packs"].push_back({{"name",p.name},{"namespace",p.meta.ns}});
    auto read=[&](pack::ResourceId const& id,char const* dir,char const* ext) {
        auto path=std::string_view(dir).starts_with("data/") ? "data/"+id.ns+"/"+dir+"/"+id.path+ext : pack::assetPath(id,dir,ext);
        auto text=packs.readFile(path);
        if (!text) throw std::runtime_error("missing "+id.toString());
        return pack::parseJsonText(*text);
    };
    std::map<pack::ResourceId,std::pair<json,std::string>> indexes;
    std::vector<fs::path> folders;
    for (auto const& folder:fs::directory_iterator(fs::path(argv[1])))
        if (folder.is_directory() && fs::exists(folder.path()/"gunpack.meta.json")) folders.push_back(folder.path());
    std::sort(folders.begin(),folders.end());
    for (auto const& folder:folders) {
        auto source=pack::openFolderPack(folder);
        for (auto const& path:source->list("data/")) {
            auto middle=path.find("/index/guns/");
            if (middle==std::string::npos || !path.ends_with(".json")) continue;
            auto id=pack::ResourceId::parse(path.substr(5,middle-5)+":"+path.substr(middle+12,path.size()-middle-17));
            if (!id) continue;
            try { indexes[*id]={pack::parseJsonText(*source->read(path)),source->name()}; }
            catch (std::exception const& e) { report["messages"].push_back(path+": "+e.what()); }
        }
    }
    for (auto const& [id,index]:indexes) {
        json row={{"id",id.toString()},{"pack",index.second},{"index",index.first},{"loaded",packs.gun(id)!=nullptr}};
        try {
            auto i=pack::parseGunIndex(index.first);
            if (i.data) row["data"]=read(*i.data,"data/guns",".json");
            if (auto g=packs.gun(id)) {
                auto initial=gun::initialAmmo(g->data);
                row["initial"]={{"magazine",initial.magazine},{"rawChamber",initial.barrel},
                    {"effectiveChamber",gun::hasChamberedRound(g->data.bolt,initial.barrel)}};
            }
            row["missing"]=packs.assetIssues().contains(id) ? json(packs.assetIssues().at(id)) : json::array();
            auto d=i.display ? packs.gunDisplay(*i.display) : nullptr;
            row["displayLoaded"]=d!=nullptr;
            if (d) {
                row["display"]=d->raw;
                if (d->model) {
                    try {
                        auto raw=read(*d->model,"geo_models",".json");
                        auto geo=model::parseGeoModel(raw);
                        auto rigid=model::compileRigidGunModel(geo);
                        row["geometry"]={{"bones",geo.bones.size()},{"groups",rigid.groups.size()},
                            {"instanceBones",rigid.magazineTraversal.size()},{"magazine",geo.find("magazine")>=0},
                            {"additional",geo.find("additional_magazine")>=0},{"thin",json::array()},{"illuminated",json::array()}};
                        for (auto const& bone:geo.bones) if (bone.illuminated && !bone.name.empty()) row["geometry"]["illuminated"].push_back(bone.name);
                        for (auto const& bone:raw.at("minecraft:geometry").at(0).at("bones")) {
                            if (!bone.contains("cubes")) continue;
                            for (auto const& cube:bone.at("cubes")) if (cube.contains("size")) {
                                auto const& s=cube.at("size");
                                float small=std::min({s.at(0).get<float>(),s.at(1).get<float>(),s.at(2).get<float>()});
                                if (small<=.0011f) row["geometry"]["thin"].push_back({{"bone",bone.value("name","")},{"minPixels",small}});
                            }
                        }
                        // Exact full-quad coincidences at rest (rounded to 1e-7 blocks).
                        // This does not claim arbitrary polygon overlap or GPU depth behavior.
                        std::map<std::array<int64_t,12>,size_t> faces;
                        auto vertices=model::bake(geo,[](auto const& b,auto* p) { return model::gunVisibility(b,p,{}); });
                        size_t coincident=0;
                        for (size_t q=0;q+4<=vertices.size();q+=4) {
                            if (glm::length(glm::cross(vertices[q+1].pos-vertices[q].pos,vertices[q+2].pos-vertices[q].pos))<1e-12f) continue;
                            std::array<std::array<int64_t,3>,4> corners;
                            for (int k=0;k<4;++k) for (int axis=0;axis<3;++axis) corners[k][axis]=std::llround(vertices[q+k].pos[axis]*1e7);
                            std::sort(corners.begin(),corners.end());
                            std::array<int64_t,12> key;
                            for (int k=0;k<4;++k) for (int a=0;a<3;++a) key[k*3+a]=corners[k][a];
                            coincident+=faces[key]++;
                        }
                        row["geometry"]["coincidentQuads"]=coincident;
                    } catch (std::exception const& e) { row["geometryError"]=e.what(); }
                }
                try {
                    auto clips=pack::loadGunAnimations(packs,*d,[&](auto const& s) { row["animationWarnings"].push_back(s); });
                    row["clips"]=json::object();
                    for (auto const& [name,clip]:clips) {
                        json c={{"duration",clip.duration},{"bones",json::array()},{"zeroScale",json::array()},{"rotations",json::array()}};
                        std::set<std::string> names;
                        for (auto const& channel:clip.channels) {
                            names.insert(channel.bone);
                            bool nonzero=false,zero=false;
                            for (auto const& key:channel.keys) {
                                nonzero|=key.pre!=glm::vec3(0) || key.post!=glm::vec3(0);
                                zero|=key.pre==glm::vec3(0) || key.post==glm::vec3(0);
                            }
                            if (channel.type==animation::ChannelType::Scale && zero) c["zeroScale"].push_back(channel.bone);
                            if (channel.type==animation::ChannelType::Rotation && nonzero) c["rotations"].push_back(channel.bone);
                        }
                        for (auto const& n:names) c["bones"].push_back(n);
                        row["clips"][name]=std::move(c);
                    }
                } catch (std::exception const& e) { row["animationError"]=e.what(); }
            }
            if (auto g=packs.gun(id); g && g->data.ammoId) if (auto ammo=packs.ammo(*g->data.ammoId); ammo && ammo->index.display) {
                try { row["ammoDisplay"]=read(*ammo->index.display,"display/ammo",".json"); }
                catch (std::exception const& e) { row["ammoError"]=e.what(); }
            }
        } catch (std::exception const& e) { row["error"]=e.what(); }
        report["guns"].push_back(std::move(row));
    }
    report["loadedCount"]=packs.guns().size();
    report["ammoCount"]=packs.ammos().size();
    report["scriptCount"]=packs.dataScripts().size();
    std::ofstream out(fs::path(argv[2]),std::ios::binary);
    out<<report.dump(2);
    if (!out) return 1;
    std::cout<<"audit: "<<indexes.size()<<" indexes, "<<packs.guns().size()<<" loaded, "<<packs.ammos().size()<<" ammo; metadata written\n";
}
