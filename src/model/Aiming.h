#pragma once

#include <optional>
#include <glm/glm.hpp>
#include "pack/GunData.h"

namespace tacz::model {

// Called once per real gameplay tick, not once per rendered frame. TACZ adds 1 ms
// to the measured tick interval; both sides use their own clocks and ads cache value.
float advanceAimProgress(float progress, bool aiming, double elapsedMs, float adsSeconds);
float effectiveAdsTime(float gunSeconds, std::optional<float> cachedAds = {});
bool fullyAimed(float progress);
float magnificationToFov(float baseDegrees, float magnification);
float aimingFov(float baseDegrees, float zoom, float progress);
float aimingSensitivity(float baseDegrees, float zoom, float progress, float screenDistance = 1.33f, float multiplier = 1.f);
float movementModifier(pack::MoveSpeed const& speed, bool aiming, bool reloading);

// Java SecondOrderDynamics: same k1/k2/k3, 0.05 simulation step, ~6 ms cadence,
// and one-step velocity prediction. Deterministic, no Java-style worker threads.
class AimDynamics {
public:
    AimDynamics(float f = 1.2f, float z = 1.2f, float r = .5f);
    void reset(float value = 0);
    float update(float target, double elapsedSeconds);
private:
    float k1, k2, k3;
    float x = 0, y = 0, velocity = 0;
    double accumulated = 0;
};

// Rigid translation interpolation + relative-quaternion slerp, not matrix element lerp.
glm::mat4 blendAimViews(glm::mat4 const& idleInverse, glm::mat4 const& ironInverse, float progress);
glm::mat4 firstPersonAimTransform(glm::mat4 const& idleInverse, glm::mat4 const& ironInverse, float progress);
// Only edits a valid perspective projection, retaining aspect, depth and off-centre terms.
float projectionFov(glm::mat4 const& projection);
glm::mat4 withModelFov(glm::mat4 projection, float degrees);

} // namespace tacz::model
