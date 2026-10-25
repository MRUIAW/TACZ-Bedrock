#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <glm/gtc/matrix_transform.hpp>
#include "model/RigidGunModel.h"
#include "model/GunModelRules.h"
#include "model/MeshDiagnostics.h"
#include "model/PlayerArms.h"
#include "pack/GunAnimations.h"
#include "pack/GunPackParser.h"

using namespace tacz;
namespace {
float normalDelta(std::vector<model::Vertex> const& a,std::vector<model::Vertex> const& b) {
    float maximum=0;
    for (size_t i=0;i<a.size();++i) for (int k=0;k<3;++k) maximum=std::max(maximum,std::abs(a[i].normal[k]-b[i].normal[k]));
    return maximum;
}
auto visibility(model::GunRenderState state={}) {
    return [state](auto const& bone,auto* parent) { return model::gunVisibility(bone,parent,state); };
}
}
TEST_CASE("Rigid groups keep named/visibility boundaries, DFS order and immutable helper geometry") {
    auto geo=model::parseGeoModel(pack::parseJsonText(R"({"format_version":"1.12.0","minecraft:geometry":[{
      "description":{"texture_width":16,"texture_height":16},"bones":[
      {"name":"root","pivot":[0,0,0],"cubes":[{"origin":[0,0,0],"size":[1,2,3],"uv":[0,0],"pivot":[1,2,3],"rotation":[13,27,39]}]},
      {"name":"mag_standard","parent":"root","pivot":[1,2,3],"cubes":[{"origin":[0,0,0],"size":[1,2,3],"uv":[0,0]}]},
      {"name":"additional_magazine","parent":"root","pivot":[0,0,0],"cubes":[{"origin":[2,0,0],"size":[1,1,1],"uv":[0,0]}]},
      {"name":"lefthand_pos","parent":"root","pivot":[1,2,3]},
      {"name":"attachment_adapter","parent":"root","pivot":[0,0,0],"cubes":[{"origin":[0,0,0],"size":[1,1,1],"uv":[0,0],"pivot":[1,2,3],"rotation":[20,30,40]}]}]}]})"));
    auto rigid=model::compileRigidGunModel(geo);
    auto saved=rigid.groups.front().vertices;
    model::BoneFrame frame;
    for (int i=0;i<50;++i) {
        animation::Pose pose;
        pose["root"].rotation=glm::vec3(.01f*i,.02f*i,-.03f*i);
        pose["root"].scale=glm::vec3(1.3f,.7f,1.1f);
        pose["mag_standard"].translation=glm::vec3(.2f,-.4f,.1f);
        model::GunRenderState state; state.extendedMagLevel=i%4; state.additionalMagazine=i%2;
        auto visit=visibility(state);
        model::evaluateBoneFrame(geo,rigid,pose,visit,frame);
        auto a=model::bake(geo,visit,pose,true),b=model::flattenRigidGunModel(rigid,frame);
        REQUIRE(a.size()==b.size()); CHECK(model::positionDelta(a,b)<2e-6f); CHECK(normalDelta(a,b)<2e-5f);
        for (size_t k=0;k<a.size();++k) CHECK(a[k].uv==b[k].uv);
        CHECK(model::positionHash(saved)==model::positionHash(rigid.groups.front().vertices));
        auto oldArm=model::playerArmTransform(geo,geo.path(geo.find("lefthand_pos")),pose);
        auto newArm=model::playerArmTransformFromFrame(geo,geo.path(geo.find("lefthand_pos")),frame);
        REQUIRE(oldArm); REQUIRE(newArm); CHECK(model::matrixDelta(*oldArm,*newArm)==0);
    }
    animation::Pose invalid; invalid[""].rotation=glm::vec3(.1f);
    CHECK_FALSE(model::rigidGunPoseCompatible(invalid));
}

TEST_CASE("Rigid draw predicate skips fully collapsed hides, preserves negative/nonuniform and planar scales") {
    model::GeoModel geo;
    model::Bone root; root.name="root"; root.quads.resize(1);
    root.quads[0].pos={glm::vec3(0),glm::vec3(16,0,0),glm::vec3(16,16,0),glm::vec3(0,16,0)};
    root.quads[0].normal={0,0,1};
    geo.bones.push_back(root); geo.roots.push_back(0);
    auto rigid=model::compileRigidGunModel(geo); model::BoneFrame frame;
    for (auto scale:{glm::vec3(0),glm::vec3(1,2,0),glm::vec3(-1,2,.7f)}) {
        animation::Pose pose; pose["root"].scale=scale;
        model::evaluateBoneFrame(geo,rigid,pose,visibility(),frame);
        CHECK(model::drawableRigidBone(frame,0)==(scale!=glm::vec3(0)));
        CHECK(model::positionDelta(model::bake(geo,visibility(),pose),model::flattenRigidGunModel(rigid,frame))==0);
    }
    frame.world[0][0][0]=std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(model::drawableRigidBone(frame,0));
    CHECK_FALSE(model::drawableRigidBone(frame,-1));
    CHECK_FALSE(model::drawableRigidBone(frame,9));
}

#ifdef TACZ_DEFAULT_PACK_DIR
TEST_CASE("Real AK47 all clips and blends: cached local meshes equal legacy baked positions/normals/UVs and arm matrices") {
    std::ifstream input(std::filesystem::path(TACZ_DEFAULT_PACK_DIR)/"assets/tacz/geo_models/gun/ak47_geo.json");
    auto geo=model::parseGeoModel(nlohmann::json::parse(input));
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(TACZ_DEFAULT_PACK_DIR));
    pack::GunPackRegistry packs; packs.load(std::move(sources),[](auto const&){},[](auto const&){});
    auto display=packs.gunDisplay({"tacz","ak47_display"}); REQUIRE(display);
    auto clips=pack::loadGunAnimations(packs,*display);
    auto rigid=model::compileRigidGunModel(geo);
    model::BoneFrame frame;
    size_t cubeBones=0,localVertices=0,visibleGroups=0,samples=0,compared=0;
    size_t singularVertices=0,minDraws=std::numeric_limits<size_t>::max(),maxDraws=0;
    float maxPosition=0,maxNormal=0,maxLocal=0;
    for (auto const& b:geo.bones) cubeBones+=!b.quads.empty();
    std::vector<uint64_t> hashes;
    for (auto const& g:rigid.groups) {
        localVertices+=g.vertices.size(); hashes.push_back(model::positionHash(g.vertices));
        for (auto const& v:g.vertices) for (int k=0;k<3;++k) maxLocal=std::max(maxLocal,std::abs(v.pos[k]));
    }
    auto check=[&](animation::Pose const& pose,model::GunRenderState state) {
        REQUIRE(model::rigidGunPoseCompatible(pose));
        auto visit=visibility(state);
        model::evaluateBoneFrame(geo,rigid,pose,visit,frame);
        size_t draws=0;
        for (auto const& g:rigid.groups) draws+=model::drawableRigidBone(frame,g.bone);
        minDraws=std::min(minDraws,draws); maxDraws=std::max(maxDraws,draws);
        auto a=model::bake(geo,visit,pose,true),b=model::flattenRigidGunModel(rigid,frame);
        REQUIRE(a.size()==b.size());
        maxPosition=std::max(maxPosition,model::positionDelta(a,b));
        float nError=0;
        size_t offset=0;
        for (auto const& group:rigid.groups) if (frame.visible[group.bone]) {
            // Zero-scale hide keys have no invertible normal matrix. Old bake's
            // fallback original normal is frame-dependent and not a shading contract.
            if (std::abs(glm::determinant(glm::mat3(frame.world[group.bone])))>1e-12f) {
                for (size_t i=0;i<group.vertices.size();++i) for (int k=0;k<3;++k)
                    nError=std::max(nError,std::abs(a[offset+i].normal[k]-b[offset+i].normal[k]));
            } else singularVertices+=group.vertices.size();
            offset+=group.vertices.size();
        }
        maxNormal=std::max(maxNormal,nError);
        CHECK(model::positionDelta(a,b)<2e-6f); CHECK(nError<2e-5f);
        bool sameUv=true;
        for (size_t i=0;i<a.size();++i) sameUv&=a[i].uv==b[i].uv;
        CHECK(sameUv);
        for (auto name:{"lefthand_pos","righthand_pos"}) {
            auto path=geo.path(geo.find(name));
            auto old=model::playerArmTransform(geo,path,pose),now=model::playerArmTransformFromFrame(geo,path,frame);
            REQUIRE(old.has_value()==now.has_value());
            if (old) CHECK(model::matrixDelta(*old,*now)==0);
        }
        ++samples; compared+=a.size();
    };
    for (auto const& [name,clip]:clips) for (int i=0;i<=32;++i) {
        INFO(name," sample ",i);
        animation::Pose pose; animation::evaluate(clip,clip.duration*i/32.f,pose);
        model::GunRenderState state; state.extendedMagLevel=i%4; state.bulletInBarrel=i%2; state.ammoCount=i%3;
        state.additionalMagazine=model::additionalMagazineKeyed(pose);
        check(pose,state);
        animation::Pose base; animation::evaluate(clips.at("static_idle"),0,base);
        animation::compose(pose,base,true); check(base,state);
    }
    model::evaluateBoneFrame(geo,rigid,{},visibility(),frame);
    for (auto const& g:rigid.groups) visibleGroups+=frame.visible[g.bone]!=0;
    for (size_t i=0;i<rigid.groups.size();++i) CHECK(hashes[i]==model::positionHash(rigid.groups[i].vertices));
    // Pure preparation timings, not native tessellation/upload/submission or GPU time.
    animation::Pose shot; animation::evaluate(clips.at("shoot"),.0333f,shot);
    constexpr int N=1000;
    auto at=std::chrono::steady_clock::now(); size_t preventElision=0;
    for (int i=0;i<N;++i) preventElision+=model::bake(geo,visibility(),shot).size();
    double oldMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-at).count()/N;
    at=std::chrono::steady_clock::now();
    for (int i=0;i<N;++i) { model::evaluateBoneFrame(geo,rigid,shot,visibility(),frame); preventElision+=frame.world.size(); }
    double newMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-at).count()/N;
    CHECK(preventElision>0);
    std::cout<<"[rigid AK47] bones="<<geo.bones.size()<<" cubeBones="<<cubeBones<<" groups="<<rigid.groups.size()
        <<" visibleGroups="<<visibleGroups<<" localVertices="<<localVertices<<" maxLocalBlocks="<<maxLocal
        <<" clips="<<clips.size()<<" samples="<<samples<<" comparedVertices="<<compared<<" maxPositionError="<<maxPosition
        <<" maxNormalError="<<maxNormal<<" singularNormalVerticesExcluded="<<singularVertices<<" sampledDrawsMin="<<minDraws<<" sampledDrawsMax="<<maxDraws
        <<" oldBakeMeanMs="<<oldMs<<" newMatricesMeanMs="<<newMs<<" nativeGPUExcluded=true\n";
}
#endif
