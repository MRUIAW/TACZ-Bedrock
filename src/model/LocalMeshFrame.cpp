#include "model/LocalMeshFrame.h"
#include <glm/gtc/packing.hpp>
#include <algorithm>
#include <cmath>
namespace tacz::model {
glm::vec3 localMeshOrigin(std::span<Vertex const> vertices) {
    glm::vec3 origin(0);
    if (vertices.empty()) return origin;
    auto lo=vertices.front().pos,hi=lo;
    for (auto const& v:vertices) { lo=glm::min(lo,v.pos); hi=glm::max(hi,v.pos); }
    for (int axis=0;axis<3;++axis) {
        float center=(lo[axis]+hi[axis])*.5f;
        if (!std::isfinite(center) || std::abs(center)>1024 || std::abs(center)<=.125f || std::abs(center)<=4*(hi[axis]-lo[axis])) continue;
        float shift=std::round(center*16.f)/16.f,before=0,after=0;
        for (auto const& v:vertices) {
            float p=v.pos[axis];
            before=std::max(before,std::abs(glm::unpackHalf1x16(glm::packHalf1x16(p))-p));
            after=std::max(after,std::abs((glm::unpackHalf1x16(glm::packHalf1x16(p-shift))+shift)-p));
        }
        if (after<before*.5f) origin[axis]=shift;
    }
    return origin;
}
}
