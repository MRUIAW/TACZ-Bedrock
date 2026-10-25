#include <doctest/doctest.h>
#include <bitset>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <glm/gtc/matrix_transform.hpp>
#include "model/FunctionalBones.h"
#include "model/MeshDiagnostics.h"
#include "model/SprintInput.h"
#include "model/GunModelRules.h"
#include "pack/GunAnimations.h"
#include "pack/GunPackParser.h"

using namespace tacz;

TEST_CASE("Sprint mask preserves held/released latches, unrelated bits and exceptions") {
    for (bool rawHeld:{false,true}) for (bool processedHeld:{false,true}) {
        std::bitset<16> raw,processed;
        raw.set(8,rawHeld); processed.set(8,processedHeld); raw.set(2);
        auto update=[&] {
            CHECK_FALSE(raw.test(8)); CHECK_FALSE(processed.test(8));
            CHECK(raw.test(2)); processed.set(3);
        };
        model::withSprintMasked(raw,processed,8,update);
        CHECK(raw.test(8)==rawHeld); CHECK(processed.test(8)==processedHeld); CHECK(processed.test(3));
        CHECK_THROWS_AS(model::withSprintMasked(raw,processed,8,[&] { update(); throw 42; }),int);
        CHECK(raw.test(8)==rawHeld); CHECK(processed.test(8)==processedHeld);
        // Once masking ends, vanilla sees the original held state, not a forced sprint.
        CHECK(raw.test(8)==rawHeld);
    }
}

TEST_CASE("Render camera component: native camera-to-world convention, roll and restore without accumulation") {
    for (auto angles:{glm::vec3(0),glm::vec3(.3f,1.2f,.2f),glm::vec3(-.8f,-2.f,-.3f)}) {
        auto view=glm::rotate(glm::mat4(1),angles.z,{0,0,1});
        view=glm::rotate(view,angles.x,{1,0,0}); view=glm::rotate(view,angles.y,{0,1,0});
        auto before=glm::quat_cast(glm::transpose(glm::mat3(view)));
        CHECK(model::cameraEntityOrientation(before,{1,0,0,0})==before);
        auto animation=model::cameraBoneRotation({.01f,.02f,.03f});
        auto after=model::cameraEntityOrientation(before,animation);
        auto expected=model::applyCameraAngles(view,animation);
        CHECK(model::matrixDelta(glm::transpose(glm::mat4_cast(after)),expected)<1e-5f);
        CHECK(glm::dot(after,before)>0); // hemisphere continuity
        CHECK(model::matrixDelta(glm::mat4_cast(model::cameraEntityOrientation(-before,animation)),glm::mat4_cast(after))<1e-5f);
        for (int frame=0;frame<120;++frame) {
            auto component=before;
            component=model::cameraEntityOrientation(component,animation);
            CHECK(std::abs(glm::dot(component,after))==doctest::Approx(1));
            component=before; // scoped restoration, original is not fed back with animation
            CHECK(component==before);
        }
    }
    auto roll=model::cameraEntityOrientation({1,0,0,0},model::cameraBoneRotation({0,0,.1f}));
    CHECK(std::abs(roll.z)>0.04f);
    CHECK(std::abs(roll.x)<1e-6f); CHECK(std::abs(roll.y)<1e-6f);
}

TEST_CASE("Mesh position diagnostics ignore UV/normals and detect topology changes") {
    std::vector<model::Vertex> a(4),b=a;
    CHECK(model::positionHash(a)==model::positionHash(b)); CHECK(model::positionDelta(a,b)==0);
    b[0].uv={.2,.3}; b[0].normal={1,0,0};
    CHECK(model::positionHash(a)==model::positionHash(b));
    b[1].pos.x=.125f;
    CHECK(model::positionHash(a)!=model::positionHash(b)); CHECK(model::positionDelta(a,b)==.125f);
    b.pop_back(); CHECK(model::positionDelta(a,b)==-1);
    b=a; b[0].pos.x=std::numeric_limits<float>::quiet_NaN();
    CHECK(std::isinf(model::positionDelta(a,b)));
    auto invalid=glm::mat4(1); invalid[0][0]=std::numeric_limits<float>::infinity();
    CHECK(std::isinf(model::matrixDelta(invalid,invalid)));
}

#ifdef TACZ_DEFAULT_PACK_DIR
TEST_CASE("Real AK47 numerical evidence: small model-space deterministic bake versus authored motion") {
    auto root=std::filesystem::path(TACZ_DEFAULT_PACK_DIR);
    std::ifstream input(root/"assets/tacz/geo_models/gun/ak47_geo.json");
    auto geo=model::parseGeoModel(nlohmann::json::parse(input));
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(TACZ_DEFAULT_PACK_DIR));
    pack::GunPackRegistry packs; packs.load(std::move(sources),[](auto const&){},[](auto const&){});
    auto display=packs.gunDisplay({"tacz","ak47_display"}); REQUIRE(display);
    auto clips=pack::loadGunAnimations(packs,*display);
    auto visit=[](auto const& bone,auto* parent) { return model::gunVisibility(bone,parent,model::GunRenderState{}); };
    animation::Pose pose; animation::evaluate(clips.at("shoot"),.0333f,pose);
    auto fixed=model::bake(geo,visit,pose);
    float maxAbs=0,repeatDelta=0,consecutive=0,edgeDelta=0;
    for (auto const& v:fixed) for (int axis=0;axis<3;++axis) maxAbs=std::max(maxAbs,std::abs(v.pos[axis]));
    auto hash=model::positionHash(fixed);
    for (int i=0;i<240;++i) {
        auto repeat=model::bake(geo,visit,pose);
        CHECK(model::positionHash(repeat)==hash);
        repeatDelta=std::max(repeatDelta,model::positionDelta(fixed,repeat));
    }
    std::vector<model::Vertex> previous;
    size_t changed=0;
    for (int frame=0;frame<=120;++frame) {
        animation::Pose animated; animation::evaluate(clips.at("shoot"),frame/240.f,animated);
        auto vertices=model::bake(geo,visit,animated);
        if (!previous.empty()) {
            consecutive=std::max(consecutive,model::positionDelta(previous,vertices));
            changed+=model::positionHash(previous)!=model::positionHash(vertices);
            for (size_t i=0;i+3<vertices.size();i+=4) for (int k=0;k<4;++k)
                edgeDelta=std::max(edgeDelta,std::abs(glm::length(vertices[i+k].pos-vertices[i+(k+1)%4].pos)-glm::length(previous[i+k].pos-previous[i+(k+1)%4].pos)));
        }
        previous=std::move(vertices);
    }
    CHECK(repeatDelta==0); CHECK(maxAbs<8); CHECK(consecutive>0); CHECK(edgeDelta<1e-5f);
    std::cout << "[AK47 CPU evidence] vertices="<<fixed.size()<<" hash="<<std::hex<<hash<<std::dec
        <<" repeatFrames=240 repeatDelta="<<repeatDelta<<" maxAbsBlocks="<<maxAbs
        <<" shootFrames=121 changedPairs="<<changed<<" consecutiveMaxBlocks="<<consecutive
        <<" quadEdgeMaxDeltaBlocks="<<edgeDelta<<'\n';
}
#endif
