#pragma once
#include <cstdint>
#include <span>
#include "model/GeoModel.h"

namespace tacz::model {
// Native default UV0: ceil(float32(x*65535)), low uint16, UNORM decode.
uint16_t packUV16(float value);
struct RepeatUVPiece {
    std::array<Vertex,4> vertices;
    glm::dvec4 domain; // s0,t0,s1,t1 in the original quad (0,1,2,3 winding)
    glm::dvec2 tile;
};
// Parser quads have rectangular, possibly reversed/zero-size UVs. Split in the
// ORIGINAL linear domain, then subtract an integer per piece; never frac endpoints.
// Throws on malformed/non-finite/unbounded input rather than silently changing it.
std::vector<RepeatUVPiece> splitRepeatUV(std::span<Vertex const,4> quad);
std::vector<Vertex> packedRepeatUVQuads(std::span<Vertex const> vertices);
} // namespace tacz::model
