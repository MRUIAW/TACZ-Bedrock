#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <limits>
#include <span>
#include "model/GeoModel.h"

namespace tacz::model {
// Hash only position float bits, never struct padding or native opaque layouts.
inline uint64_t positionHash(std::span<Vertex const> vertices) {
    uint64_t hash=14695981039346656037ull;
    for (auto const& v:vertices) for (int axis=0;axis<3;++axis) {
        auto bits=std::bit_cast<uint32_t>(v.pos[axis]);
        for (int byte=0;byte<4;++byte) { hash^=(bits>>(byte*8))&255; hash*=1099511628211ull; }
    }
    return hash;
}
inline float positionDelta(std::span<Vertex const> a, std::span<Vertex const> b) {
    if (a.size()!=b.size()) return -1; // topology changed, not a valid comparison
    float maximum=0;
    for (size_t i=0;i<a.size();++i) for (int axis=0;axis<3;++axis) {
        if (!std::isfinite(a[i].pos[axis]) || !std::isfinite(b[i].pos[axis])) return std::numeric_limits<float>::infinity();
        maximum=std::max(maximum,std::abs(a[i].pos[axis]-b[i].pos[axis]));
    }
    return maximum;
}
inline float matrixDelta(glm::mat4 const& a, glm::mat4 const& b) {
    float maximum=0;
    for (int c=0;c<4;++c) for (int r=0;r<4;++r) {
        if (!std::isfinite(a[c][r]) || !std::isfinite(b[c][r])) return std::numeric_limits<float>::infinity();
        maximum=std::max(maximum,std::abs(a[c][r]-b[c][r]));
    }
    return maximum;
}
} // namespace tacz::model
