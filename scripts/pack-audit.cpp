#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <array>
#include <cmath>
#include <cstring>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "model/GunModelRules.h"
#include "pack/GunPackLoader.h"
#include "pack/GunPackParser.h"
#include "pack/ItemTooltip.h"
#include "pack/GunAnimations.h"
#include "model/GeoModel.h"
#include "model/RigidGunModel.h"
#include "model/PackedUV.h"
#include "model/LocalMeshFrame.h"
#include "gun/ShootRules.h"

namespace {
nlohmann::json hkDepth(tacz::model::GeoModel const& geo,tacz::model::RigidGunModel const& rigid) {
    using namespace tacz;
    using json=nlohmann::json;
    model::BoneFrame frame;
    model::evaluateBoneFrame(geo,rigid,{},[](auto const&,auto*) { return model::Visit::Draw; },frame);
    int decal=geo.find("bone"),body=geo.find("gun_body2");
    if (decal<0 || body<0) return {{"error","missing target bones"}};
    json report={{"targets",json::array()},{"nativePositionHalfActive","unverified: capability dependent; half below is a bounded simulation"}};
    auto const& db=geo.bones[decal];
    auto half=[](glm::vec3 v) { for (int a=0;a<3;++a) v[a]=glm::unpackHalf1x16(glm::packHalf1x16(v[a])); return v; };
    std::map<int,glm::vec3> origins;
    for (auto const& g:rigid.groups) origins.try_emplace(g.bone,model::localMeshOrigin(g.vertices));
    for (size_t qi:{0u,2u,3u}) {
        auto const& q=db.quads.at(qi);
        auto local=q.pos[0]/16.f;
        glm::vec3 wp=frame.world[decal]*glm::vec4(local,1);
        glm::vec3 hp=frame.world[decal]*glm::vec4(half(local),1);
        auto newLocal=half(local-origins[decal])+origins[decal];
        glm::vec3 newWorld=frame.world[decal]*glm::vec4(newLocal,1);
        size_t matched=0,clipMismatches=0;
        int groupBone=-1;
        for (auto const& group:rigid.groups) if (group.bone==decal) {
            groupBone=group.bone;
            for (size_t vi=0;vi+4<=group.vertices.size();vi+=4) if (group.vertices[vi].uv==q.uv[0]) {
                ++matched;
                for (int k=0;k<4;++k) for (int camera=0;camera<256;++camera) {
                    auto clip=glm::perspective(.8f,.5625f,.01f,100.f)
                        *glm::rotate(glm::translate(glm::mat4(1),{.14f,-.3f,-1.2f}),camera*.024f,glm::normalize(glm::vec3(1,2,3)));
                    auto java=(clip*frame.world[decal])*glm::vec4(q.pos[k]/16.f,1);
                    auto native=(clip*frame.world[group.bone])*glm::vec4(group.vertices[vi+k].pos,1);
                    clipMismatches+=std::memcmp(&java.z,&native.z,sizeof(float))!=0 || std::memcmp(&java.w,&native.w,sizeof(float))!=0;
                }
            }
        }
        glm::vec3 lo(1e9f),hi(-1e9f);
        for (auto p:q.pos) { glm::vec3 w=frame.world[decal]*glm::vec4(p/16.f,1); lo=glm::min(lo,w);hi=glm::max(hi,w); }
        std::vector<json> candidates;
        for (size_t bi=0;bi<geo.bones.size();++bi) {
            if (static_cast<int>(bi)==decal) continue;
            for (size_t i=0;i<geo.bones[bi].quads.size();++i) {
                auto const& bq=geo.bones[bi].quads[i];
                glm::vec3 bl(1e9f),bh(-1e9f);
                for (auto p:bq.pos) { glm::vec3 w=frame.world[bi]*glm::vec4(p/16.f,1);bl=glm::min(bl,w);bh=glm::max(bh,w); }
                if (bh.x-bl.x>1e-7f || std::min(hi.y,bh.y)<=std::max(lo.y,bl.y) || std::min(hi.z,bh.z)<=std::max(lo.z,bl.z)) continue;
                float gap=wp.x-bl.x;
                if (std::abs(gap)>.002f) continue;
                glm::vec3 hbody=frame.world[bi]*glm::vec4(half(bq.pos[0]/16.f),1);
                glm::vec3 newBody=frame.world[bi]*glm::vec4(half(bq.pos[0]/16.f-origins[static_cast<int>(bi)])+origins[static_cast<int>(bi)],1);
                candidates.push_back({{"bone",geo.bones[bi].name},{"parsedBone",bi},{"quad",i},{"gapBlocks",gap},
                    {"absGap",std::abs(gap)},{"bodyWorldX",bl.x},{"decalHalfWorldX",hp.x},{"bodyHalfWorldX",hbody.x},{"halfGapBlocks",hp.x-hbody.x},
                    {"rebasedHalfDecalWorldX",newWorld.x},{"rebasedHalfBodyWorldX",newBody.x},{"rebasedHalfGapBlocks",newWorld.x-newBody.x}});
            }
        }
        std::sort(candidates.begin(),candidates.end(),[](auto const& a,auto const& b) { return a.at("absGap").template get<float>()<b.at("absGap").template get<float>(); });
        if (candidates.size()>4) candidates.resize(4);
        report["targets"].push_back({{"cube",qi},{"groupBone",groupBone},{"bone",db.name},{"namedBoundaryRetained",groupBone==decal},
            {"matchingQuads",matched},{"prepackClipDepthBitMismatches",clipMismatches},{"prepackClipComparisons",matched*4*256},
            {"localX",local.x},{"worldX",wp.x},{"halfWorldX",hp.x},{"origin",{origins[decal].x,origins[decal].y,origins[decal].z}},
            {"rebasedHalfLocalX",newLocal.x-origins[decal].x},{"nearbyParallelOverlaps",candidates}});
    }
    return report;
}
}

// Production parser/registry/model compiler, not a second approximate JSON parser.
// Writes metadata only to the caller's output path; never modifies a gun pack.
int main(int argc,char** argv) {
    if (argc!=3) { std::cerr<<"usage: tacz-pack-audit <gunpacks-directory> <metadata.json>\n"; return 2; }
    using namespace tacz;
    namespace fs=std::filesystem;
    using json=nlohmann::json;
    if (!fs::is_directory(fs::path(argv[1]))) { std::cerr<<"gunpacks directory does not exist\n"; return 2; }
    pack::GunPackRegistry packs;
    json report={{"input",fs::absolute(fs::path(argv[1])).lexically_normal().generic_string()},
        {"messages",json::array()},{"guns",json::array()},{"packs",json::array()}};
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
                row["item"]={{"type",pack::itemType(pack::ItemKind::Gun)},
                    {"en_us",pack::itemName(packs,{pack::ItemKind::Gun,id},"en_us")},
                    {"zh_cn",pack::itemName(packs,{pack::ItemKind::Gun,id},"zh_cn")}};
                if (auto icon=pack::itemIcon(packs,{pack::ItemKind::Gun,id})) row["item"]["slot"]=*icon;
                pack::TooltipState tooltipState;
                tooltipState.ammo=g->data.ammoAmount; tooltipState.chamber=true;
                tooltipState.mode=g->data.fireModes.empty() ? std::nullopt : g->data.fireModes.front();
                for (auto locale:{"en_us","zh_cn"}) row["item"]["tooltip"][locale]=pack::buildItemTooltip(packs,{pack::ItemKind::Gun,id},locale,tooltipState).lines;
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
                        size_t shifted=0; float roundtrip=0;
                        for (auto const& g:rigid.groups) {
                            auto origin=model::localMeshOrigin(g.vertices); shifted+=origin!=glm::vec3(0);
                            for (auto const& v:g.vertices) roundtrip=std::max(roundtrip,glm::length((v.pos-origin)+origin-v.pos));
                        }
                        row["positionFrames"]={{"rebasedGroups",shifted},{"maxLocalRoundtrip",roundtrip}};
                        if (id.toString()=="tacz:hk416d") row["decalDepth"]=hkDepth(geo,rigid);
                         row["geometry"]={{"bones",geo.bones.size()},{"groups",rigid.groups.size()},
                            {"instanceBones",rigid.magazineTraversal.size()},{"magazine",geo.find("magazine")>=0},
                             {"additional",geo.find("additional_magazine")>=0},{"thin",json::array()},{"illuminated",json::array()}};
                        json uv={{"faces",0},{"outside",0},{"pieces",0},{"verifiedPieces",0},{"maxPackedError",0.},
                            {"failures",json::array()},{"examples",json::array()}};
                        size_t faceCount=0,outside=0,pieceCount=0,verified=0,nonzeroUV=0,handLocator=0;
                        double maxError=0;
                        std::vector<json> examples;
                        for (size_t bi=0;bi<geo.bones.size();++bi) {
                            auto const& bone=geo.bones[bi];
                            for (size_t qi=0;qi<bone.quads.size();++qi) {
                                auto const& q=bone.quads[qi];
                                ++faceCount;
                                glm::vec2 lo=q.uv[0],hi=lo;
                                std::array<model::Vertex,4> vertices;
                                for (int k=0;k<4;++k) {
                                    lo=glm::min(lo,q.uv[k]); hi=glm::max(hi,q.uv[k]);
                                    vertices[k]={q.pos[k]/16.f,q.uv[k],q.normal};
                                }
                                bool overflow=lo.x<0 || lo.y<0 || hi.x>1 || hi.y>1;
                                if (!overflow) continue;
                                ++outside;
                                nonzeroUV+=hi.x>lo.x && hi.y>lo.y;
                                bool hand=false;
                                for (int ancestor=static_cast<int>(bi);ancestor>=0;ancestor=geo.bones[ancestor].parent)
                                    hand|=geo.bones[ancestor].name=="righthand_pos" || geo.bones[ancestor].name=="lefthand_pos";
                                handLocator+=hand;
                                double severity=std::max({-lo.x,-lo.y,hi.x-1,hi.y-1});
                                std::string owner=bone.name.empty() && bone.parent>=0 ? geo.bones[bone.parent].name+"/(cube helper)" : bone.name;
                                examples.push_back({{"bone",owner},{"parsedBone",bi},{"quad",qi},{"min",{lo.x,lo.y}},
                                    {"max",{hi.x,hi.y}},{"overflow",severity},{"handLocator",hand}});
                                try {
                                    auto pieces=model::splitRepeatUV(vertices);
                                    pieceCount+=pieces.size();
                                    for (auto const& p:pieces) {
                                        bool good=true;
                                        for (double s:{.01,.37,.99}) for (double t:{.01,.61,.99}) {
                                            double a=p.domain.x+(p.domain.z-p.domain.x)*s,b=p.domain.y+(p.domain.w-p.domain.y)*t;
                                            glm::dvec2 expected=glm::dvec2(q.uv[0])*(1-a)*(1-b)+glm::dvec2(q.uv[1])*a*(1-b)
                                                +glm::dvec2(q.uv[2])*a*b+glm::dvec2(q.uv[3])*(1-a)*b;
                                            glm::dvec2 decoded(0);
                                            for (int k=0;k<4;++k) {
                                                auto const& v=p.vertices[k];
                                                good&=v.uv.x>=0 && v.uv.x<=1 && v.uv.y>=0 && v.uv.y<=1;
                                                double w=(k==0 || k==3 ? 1-s : s)*(k<2 ? 1-t : t);
                                                for (int axis=0;axis<2;++axis) decoded[axis]+=w*model::packUV16(v.uv[axis])/65535.;
                                            }
                                            auto error=glm::abs(decoded+p.tile-expected);
                                            maxError=std::max({maxError,error.x,error.y});
                                            good&=error.x<2./65535 && error.y<2./65535;
                                        }
                                        if (good) ++verified;
                                        else uv["failures"].push_back({{"bone",owner},{"quad",qi},{"reason","packed interpolation"}});
                                    }
                                } catch (std::exception const& e) { uv["failures"].push_back({{"bone",owner},{"quad",qi},{"reason",e.what()}}); }
                            }
                        }
                        std::sort(examples.begin(),examples.end(),[](auto const& a,auto const& b) { return a.at("overflow").template get<double>()>b.at("overflow").template get<double>(); });
                        for (size_t i=0;i<std::min<size_t>(5,examples.size());++i) uv["examples"].push_back(examples[i]);
                        uv["faces"]=faceCount; uv["outside"]=outside; uv["pieces"]=pieceCount;
                        uv["verifiedPieces"]=verified; uv["maxPackedError"]=maxError;
                        uv["outsideNonzeroUV"]=nonzeroUV; uv["outsideHandLocator"]=handLocator;
                        row["geometry"]["uv"]=std::move(uv);
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
    report["ammoItems"]=json::array();
    for (auto const& [id,ammo]:packs.ammos()) {
        json a={{"id",id.toString()},{"stackDeclared",ammo.index.stackSize},{"stackBedrock",pack::ammoStackLimit(ammo.index.stackSize)},
            {"en_us",pack::itemName(packs,{pack::ItemKind::Ammo,id},"en_us")},{"zh_cn",pack::itemName(packs,{pack::ItemKind::Ammo,id},"zh_cn")}};
        if (auto slot=pack::itemIcon(packs,{pack::ItemKind::Ammo,id})) a["slot"]=*slot;
        a["give120"]=pack::splitAmmo(120,ammo.index.stackSize);
        a["tooltip_zh_cn"]=pack::buildItemTooltip(packs,{pack::ItemKind::Ammo,id},"zh_cn").lines;
        report["ammoItems"].push_back(std::move(a));
    }
    std::ofstream out(fs::path(argv[2]),std::ios::binary);
    out<<report.dump(2);
    if (!out) return 1;
    std::cout<<"audit: "<<indexes.size()<<" indexes, "<<packs.guns().size()<<" loaded, "<<packs.ammos().size()<<" ammo; metadata written\n";
}
