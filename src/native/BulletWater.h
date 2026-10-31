#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "mc/deps/shared_types/v1_26_20/block/MaterialType.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/phys/AABB.h"

namespace tacz::native {
// Sample at SIMULATED tick-start position, not the (deliberately lagging) native actor.
// Java's 1/16 entity AABB deflated .001; Bedrock supplies liquid intersection semantics.
inline bool bulletInWater(BlockSource& region, glm::dvec3 p) {
    constexpr double half = .03125, height = .0625, epsilon = .001;
    Vec3 min{static_cast<float>(p.x-half+epsilon), static_cast<float>(p.y+epsilon), static_cast<float>(p.z-half+epsilon)};
    Vec3 max{static_cast<float>(p.x+half-epsilon), static_cast<float>(p.y+height-epsilon), static_cast<float>(p.z+half-epsilon)};
    if (!region.hasChunksAt(BlockPos{min}, 1, false)) return false;
    return Actor::isInLiquid(SharedTypes::v1_26_20::MaterialType::Water, region, AABB{min,max}, {});
}
}
