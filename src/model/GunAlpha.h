#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include "model/GeoModel.h"

namespace tacz::model {
enum class AlphaClass { Opaque, Cutout, Fractional };
// Java rendertype_entity_cutout.fsh discards sampled alpha < .1. Native moving
// block alpha-test has a different backend-dependent cutoff. A binary mask keeps
// accepted RGB texels on that known-working route (filter-edge equivalence is approximate).
inline void javaCutoutMask(std::span<uint8_t> rgba) {
    for (size_t i=3;i<rgba.size();i+=4) rgba[i]=rgba[i]>=26 ? 255 : 0;
}
inline AlphaClass groupAlpha(std::span<Vertex const> vertices,std::span<uint8_t const> rgba,int w,int h) {
    if (w<=0 || h<=0 || rgba.size()!=size_t(w)*h*4 || vertices.size()%4) return AlphaClass::Fractional;
    bool zero=false;
    for (size_t q=0;q+4<=vertices.size();q+=4) {
        glm::vec2 lo=vertices[q].uv,hi=lo;
        for (int k=1;k<4;++k) { lo=glm::min(lo,vertices[q+k].uv); hi=glm::max(hi,vertices[q+k].uv); }
        if (!std::isfinite(lo.x) || !std::isfinite(lo.y) || !std::isfinite(hi.x) || !std::isfinite(hi.y)) return AlphaClass::Fractional;
        // Repeat addressing, one-texel conservative filter border. Huge/repeated
        // UV spans inspect the whole image rather than doing unbounded work.
        int x0=0,x1=w-1,y0=0,y1=h-1;
        if (hi.x-lo.x<1 && std::abs(lo.x)<1000 && std::abs(hi.x)<1000) { x0=int(std::floor(lo.x*w))-1; x1=int(std::ceil(hi.x*w)); }
        if (hi.y-lo.y<1 && std::abs(lo.y)<1000 && std::abs(hi.y)<1000) { y0=int(std::floor(lo.y*h))-1; y1=int(std::ceil(hi.y*h)); }
        for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x) {
            auto a=rgba[(size_t((y%h+h)%h)*w+(x%w+w)%w)*4+3];
            if (a>0 && a<255) return AlphaClass::Fractional;
            zero|=a==0;
        }
    }
    return zero ? AlphaClass::Cutout : AlphaClass::Opaque;
}
} // namespace tacz::model
