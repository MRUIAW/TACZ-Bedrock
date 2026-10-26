#include <doctest/doctest.h>
#include "model/GeoModel.h"
#include "pack/GunPackParser.h"

using namespace tacz;
TEST_CASE("TACZ per-face UV: exact installed decal endpoints, negative size and up/down, no inset") {
    auto raw=pack::parseJsonText(R"({"format_version":"1.12.0","minecraft:geometry":[{"description":{"texture_width":256,"texture_height":256},"bones":[{"name":"labels","pivot":[0,0,0],"cubes":[{"origin":[0,0,0],"size":[1,1,1],"uv":{
        "west":{"uv":[217,45.4688],"uv_size":[12.5313,2.5625]},
        "east":{"uv":[229.53125,35.96875],"uv_size":[-13.5625,3.0625]},
        "up":{"uv":[8,16],"uv_size":[-4,-8]},
        "down":{"uv":[32,64],"uv_size":[4,8]},
        "north":{"uv":[216.96875,52.96875],"uv_size":[10.0625,4.0625]}
    }}]}]}]})");
    auto geo=model::parseGeoModel(raw);
    auto const& quads=geo.bones[0].quads;
    REQUIRE(quads.size()==5);
    // Independent transcription of BedrockPolygon.java:14-17 (f=f1=0).
    std::array<glm::vec4,5> rects{{{217,45.4688f,229.5313f,48.0313f},
        {229.53125f,35.96875f,215.96875f,39.03125f},{8,16,4,8},{32,64,36,72},
        {216.96875f,52.96875f,227.03125f,57.03125f}}};
    for (size_t i=0;i<rects.size();++i) {
        auto r=rects[i]/256.f;
        std::array<glm::vec2,4> java{{{r.z,r.y},{r.x,r.y},{r.x,r.w},{r.z,r.w}}};
        for (size_t k=0;k<4;++k) {
            CHECK(quads[i].uv[k].x==doctest::Approx(java[k].x));
            CHECK(quads[i].uv[k].y==doctest::Approx(java[k].y));
        }
    }
    // up/down JSON is routed to DOWN/UP geometry faces, not an extra UV inversion.
    CHECK(quads[2].normal==glm::vec3(0,-1,0));
    CHECK(quads[3].normal==glm::vec3(0,1,0));
    CHECK(quads[0].uv[0].x*512==doctest::Approx(459.0626));
    CHECK(quads[1].uv[1].x*512==doctest::Approx(459.0625));
}
