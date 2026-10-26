#include <doctest/doctest.h>
#include <glm/gtc/packing.hpp>
#include "model/LocalMeshFrame.h"
using namespace tacz::model;
TEST_CASE("Far-pivot HK decal origin keeps authored inflate through half position packing") {
    std::vector<Vertex> v={{{-.33978125453f,0,0},{},{1,0,0}},{{-.33196875453f,0,0},{},{1,0,0}},
        {{-.41828000546f,0,0},{},{-1,0,0}}};
    auto origin=localMeshOrigin(v);
    CHECK(origin==glm::vec3(-.375f,0,0));
    for (size_t i=0;i<2;++i) {
        float body=i==0 ? .03515625f : .04296875f;
        float packed=glm::unpackHalf1x16(glm::packHalf1x16(v[i].pos.x-origin.x));
        float restored=packed+origin.x+.375f;
        CHECK(restored>body);
        CHECK(restored-body==doctest::Approx(.00006103515625));
        CHECK(glm::unpackHalf1x16(glm::packHalf1x16(v[i].pos.x))+.375f==body);
        for (float distance:{.25f,.5f,1.f,2.f}) {
            // Camera looks onto the decal side: outward positive X is nearer.
            auto projection=glm::perspective(.8f,1.7f,.01f,16.f);
            auto decal=projection*glm::vec4(0,0,-(distance-restored),1);
            auto behind=projection*glm::vec4(0,0,-(distance-body),1);
            CHECK(decal.z/decal.w<behind.z/behind.w);
            auto depth=[](glm::vec4 p) { return std::llround((p.z/p.w*.5+ .5)*16777215.); };
            CHECK(depth(decal)<depth(behind)); // simulated24-bit first-person depth
        }
    }
    auto bone=glm::rotate(glm::translate(glm::mat4(1),{.1f,.2f,-.5f}),.7f,glm::vec3(0,1,0));
    for (auto const& p:v) {
        auto before=bone*glm::vec4(p.pos,1),after=localMeshMatrix(bone,origin)*glm::vec4(p.pos-origin,1);
        CHECK(glm::length(before-after)<1e-6f);
    }
}
TEST_CASE("In-range centered gun meshes keep the old frame") {
    std::array<Vertex,2> v{{{{-.1f,-.2f,-.3f},{},{}},{{.1f,.2f,.3f},{},{}}}};
    CHECK(localMeshOrigin(v)==glm::vec3(0));
    CHECK(localMeshOrigin({})==glm::vec3(0));
}
