#pragma once

#include <cmath>

namespace tacz::animation {

// Java 1.20.1 Entity.move: walkDist += (float) horizontalDistance * 0.6F.
// Do not use Bedrock's differently defined animation-distance accumulator as this value.
class WalkDistance {
public:
    static constexpr float DistanceFactor = 0.6f;
    static float increment(double dx, double dz) {
        return static_cast<float>(std::hypot(dx, dz)) * DistanceFactor;
    }
    void tick(double horizontalDistance) {
        previous = current;
        current += static_cast<float>(horizontalDistance) * DistanceFactor;
    }
    // GunAnimationStateContext extrapolates CURRENT + (CURRENT - PREVIOUS) * partialTicks;
    // it does not lerp previous -> current. Repeated frame reads never accumulate distance.
    float predicted(float partialTicks) const { return current + (current - previous) * partialTicks; }
    float current = 0;
    float previous = 0;
};

} // namespace tacz::animation
