#include <doctest/doctest.h>
#include <cstring>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "model/RigidGunModel.h"
#include "pack/GunPackParser.h"
using namespace tacz;
TEST_CASE("HK-style named decal is not helper-folded; authored inflate is not Java coplanarity") {
    auto geo=model::parseGeoModel(pack::parseJsonText(R"({"format_version":"1.12.0","minecraft:geometry":[{
        "description":{"texture_width":256,"texture_height":256},"bones":[
        {"name":"gun_body2","pivot":[0,8,1],"cubes":[
          {"origin":[-0.5625,7.04689,3.31251],"size":[1.125,1.6875,4.43749],"uv":{"west":{"uv":[0,0],"uv_size":[1,1]}}},
          {"origin":[-0.6875,6.10939,0.18751],"size":[1.375,2.62499,3.125],"uv":{"west":{"uv":[0,0],"uv_size":[1,1]}}}]},
        {"name":"bone","parent":"gun_body2","pivot":[6,0,0],"cubes":[
          {"origin":[-0.5625,8.216,4.34765],"size":[1.125,0.187,0.93935],"inflate":0.001,"uv":{"west":{"uv":[217,45.4688],"uv_size":[12.5313,2.5625]}}},
          {"origin":[0.3125,6.38259,0.37826],"size":[0.375,0.51622,1.28122],"inflate":0.001,"uv":{"west":{"uv":[216.96875,52.96875],"uv_size":[10.0625,4.0625]}}}]}]}]})"));
    auto rigid=model::compileRigidGunModel(geo);
    REQUIRE(rigid.groups.size()==2);
    CHECK(rigid.groups[0].bone==geo.find("gun_body2"));
    CHECK(rigid.groups[1].bone==geo.find("bone"));
    model::BoneFrame frame;
    model::evaluateBoneFrame(geo,rigid,{},[](auto const&,auto*) { return model::Visit::Draw; },frame);
    for (int q=0;q<2;++q) {
        auto local=geo.bones[1].quads[q].pos[0]/16.f;
        glm::vec3 world=frame.world[1]*glm::vec4(local,1);
        auto body=geo.bones[0].quads[q].pos[0]/16.f;
        glm::vec3 bodyWorld=frame.world[0]*glm::vec4(body,1);
        CHECK(world.x-bodyWorld.x==doctest::Approx(.001f/16).epsilon(.001));
        auto quantized=local;
        quantized.x=glm::unpackHalf1x16(glm::packHalf1x16(local.x));
        glm::vec3 halfWorld=frame.world[1]*glm::vec4(quantized,1);
        CHECK(halfWorld.x==bodyWorld.x); // half simulation, NOT backend selection proof
        for (int k=0;k<4;++k) {
            auto const& v=rigid.groups[1].vertices[q*4+k];
            CHECK(v.pos==geo.bones[1].quads[q].pos[k]/16.f);
            for (int angle=0;angle<128;++angle) {
                auto clip=glm::perspective(.8f,1.7f,.01f,100.f)*glm::rotate(glm::mat4(1),angle*.017f,glm::normalize(glm::vec3(1,2,3)));
                auto expected=(clip*frame.world[1])*glm::vec4(geo.bones[1].quads[q].pos[k]/16.f,1);
                auto compiled=(clip*frame.world[1])*glm::vec4(v.pos,1);
                CHECK(std::memcmp(&expected.z,&compiled.z,sizeof(float))==0);
                CHECK(std::memcmp(&expected.w,&compiled.w,sizeof(float))==0);
            }
        }
    }
}
