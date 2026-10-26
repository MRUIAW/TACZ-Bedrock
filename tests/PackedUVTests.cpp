#include <doctest/doctest.h>
#include "model/PackedUV.h"
using namespace tacz::model;
namespace {
std::array<Vertex,4> quad(float a,float b,float c,float d) {
    return {{{{0,0,0},{a,b},{0,0,1}},{{1,0,0},{c,b},{0,0,1}},
        {{1,1,0},{c,d},{0,0,1}},{{0,1,0},{a,d},{0,0,1}}}};
}
}
TEST_CASE("Native packed UV overflow reproduces Timeless inverted whole-texture interpolation") {
    CHECK(packUV16(.7890625f)==51712);
    CHECK(packUV16(1.005859375f)==383);
    CHECK(packUV16(1)==65535);
    CHECK(packUV16(0)==0);
    CHECK(packUV16(-.25f)==49153);
    auto pieces=splitRepeatUV(quad(0,.7890625f,.533203125f,1.005859375f));
    REQUIRE(pieces.size()==2);
    CHECK(pieces[0].domain.w==doctest::Approx(36./37.));
    CHECK(pieces[0].vertices[2].uv.y==1);
    CHECK(pieces[1].vertices[0].uv.y==0);
    CHECK(pieces[1].vertices[2].uv.y==.005859375f);
    CHECK(pieces[0].vertices[2].pos==pieces[1].vertices[1].pos);
}
TEST_CASE("Packed repeat preserves reversal, winding, range, coverage and linear interpolation") {
    for (auto rect:{glm::vec4(0,.7890625f,.533203125f,1.005859375f),glm::vec4(-2.4f,3.1f,2.2f,-1.25f),
        glm::vec4(2,2,3,3),glm::vec4(-1,-1,0,0),glm::vec4(2,3,2,3),glm::vec4(.9f,.2f,.1f,.8f)}) {
        auto q=quad(rect.x,rect.y,rect.z,rect.w);
        double area=0;
        for (auto const& p:splitRepeatUV(q)) {
            area+=(p.domain.z-p.domain.x)*(p.domain.w-p.domain.y);
            for (auto const& v:p.vertices) {
                CHECK(v.uv.x>=0); CHECK(v.uv.x<=1); CHECK(v.uv.y>=0); CHECK(v.uv.y<=1);
                CHECK(v.normal==q[0].normal);
            }
            CHECK(glm::cross(p.vertices[1].pos-p.vertices[0].pos,p.vertices[3].pos-p.vertices[0].pos).z>0);
            for (double s:{.01,.4,.99}) for (double t:{.01,.6,.99}) {
                auto expected=glm::dvec2(rect.x,rect.y)+glm::dvec2(rect.z-rect.x,rect.w-rect.y)
                    *glm::dvec2(p.domain.x+(p.domain.z-p.domain.x)*s,p.domain.y+(p.domain.w-p.domain.y)*t);
                glm::dvec2 decoded(0);
                for (int k=0;k<4;++k) {
                    double weight=(k==0 || k==3 ? 1-s : s)*(k<2 ? 1-t : t);
                    for (int axis=0;axis<2;++axis) decoded[axis]+=weight*packUV16(p.vertices[k].uv[axis])/65535.;
                }
                auto error=decoded+p.tile-expected;
                CHECK(std::abs(error.x)<2./65535); CHECK(std::abs(error.y)<2./65535);
            }
        }
        CHECK(area==doctest::Approx(1));
    }
}
TEST_CASE("In-range upload is bit-identical; split is bounded and rejects malformed data") {
    auto q=quad(1,0,0,1);
    auto output=packedRepeatUVQuads(q);
    REQUIRE(output.size()==q.size());
    for (size_t i=0;i<q.size();++i) {
        CHECK(output[i].pos==q[i].pos); CHECK(output[i].uv==q[i].uv); CHECK(output[i].normal==q[i].normal);
    }
    CHECK_THROWS(splitRepeatUV(quad(0,0,20000,1)));
    CHECK_THROWS(splitRepeatUV(quad(0,0,300,300)));
    q[2].uv.x=.5f; CHECK_THROWS(splitRepeatUV(q));
    CHECK_THROWS(packedRepeatUVQuads(std::span<Vertex const>(q).first(3)));
}
