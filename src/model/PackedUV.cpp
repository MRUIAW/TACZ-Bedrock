#include "model/PackedUV.h"
#include <algorithm>
#include <cmath>

namespace tacz::model {
uint16_t packUV16(float value) {
    float scaled=value*65535.f;
    double integer=std::ceil(scaled);
    if (!std::isfinite(integer) || integer<-2147483648. || integer>2147483647.)
        throw ModelError("UV exceeds native int32 conversion range");
    return static_cast<uint16_t>(static_cast<int32_t>(integer));
}
namespace {
std::vector<double> cuts(double a,double b) {
    if (!std::isfinite(a) || !std::isfinite(b) || std::max(std::abs(a),std::abs(b))>16384)
        throw ModelError("UV repeat range exceeds bounded mesh budget");
    std::vector<double> result{0,1};
    if (a!=b) for (double k=std::floor(std::min(a,b))+1;k<std::max(a,b);++k)
        result.push_back((k-a)/(b-a));
    std::sort(result.begin(),result.end());
    return result;
}
}
std::vector<RepeatUVPiece> splitRepeatUV(std::span<Vertex const,4> q) {
    for (auto const& v:q) for (int a=0;a<2;++a)
        if (!std::isfinite(v.uv[a])) throw ModelError("non-finite packed UV");
    if (q[0].uv.y!=q[1].uv.y || q[1].uv.x!=q[2].uv.x || q[2].uv.y!=q[3].uv.y || q[3].uv.x!=q[0].uv.x)
        throw ModelError("packed repeat requires parser rectangular UVs");
    double u0=q[0].uv.x,u1=q[1].uv.x,v0=q[0].uv.y,v1=q[3].uv.y;
    auto s=cuts(u0,u1),t=cuts(v0,v1);
    if ((s.size()-1)*(t.size()-1)>65536) throw ModelError("UV split exceeds mesh budget");
    std::vector<RepeatUVPiece> out;
    for (size_t y=1;y<t.size();++y) for (size_t x=1;x<s.size();++x) {
        RepeatUVPiece piece;
        piece.domain={s[x-1],t[y-1],s[x],t[y]};
        piece.tile={std::floor(u0+(u1-u0)*(s[x-1]+s[x])*.5),std::floor(v0+(v1-v0)*(t[y-1]+t[y])*.5)};
        // Keep in-range endpoints (including exactly 1) bit-identical.
        if (u0>=0 && u0<=1 && u1>=0 && u1<=1) piece.tile.x=0;
        if (v0>=0 && v0<=1 && v1>=0 && v1<=1) piece.tile.y=0;
        std::array<glm::dvec2,4> corners{{{s[x-1],t[y-1]},{s[x],t[y-1]},{s[x],t[y]},{s[x-1],t[y]}}};
        for (int k=0;k<4;++k) {
            double a=corners[k].x,b=corners[k].y;
            auto& v=piece.vertices[k];
            // Exact original corners avoid unnecessary position/normal rounding.
            if ((a==0 || a==1) && (b==0 || b==1)) v=q[b==0 ? (a==0 ? 0 : 1) : (a==0 ? 3 : 2)];
            else {
                v.pos=glm::dvec3(q[0].pos)*(1-a)*(1-b)+glm::dvec3(q[1].pos)*a*(1-b)
                    +glm::dvec3(q[2].pos)*a*b+glm::dvec3(q[3].pos)*(1-a)*b;
                v.normal=q[0].normal; // planar source quad
            }
            v.uv={std::clamp(u0+(u1-u0)*a-piece.tile.x,0.,1.),std::clamp(v0+(v1-v0)*b-piece.tile.y,0.,1.)};
        }
        out.push_back(piece);
    }
    return out;
}
std::vector<Vertex> packedRepeatUVQuads(std::span<Vertex const> vertices) {
    if (vertices.size()%4) throw ModelError("packed UV input is not quads");
    std::vector<Vertex> out;
    out.reserve(vertices.size());
    for (size_t i=0;i<vertices.size();i+=4) {
        auto q=vertices.subspan(i,4);
        bool inside=true;
        for (auto const& v:q) inside&=v.uv.x>=0 && v.uv.x<=1 && v.uv.y>=0 && v.uv.y<=1;
        if (inside) out.insert(out.end(),q.begin(),q.end());
        else for (auto const& piece:splitRepeatUV(std::span<Vertex const,4>(q)))
            out.insert(out.end(),piece.vertices.begin(),piece.vertices.end());
    }
    return out;
}
} // namespace tacz::model
