#include "model/Aiming.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace tacz::model {
namespace {
float progressValue(float p) { return std::isfinite(p) ? std::clamp(p, 0.f, 1.f) : 0.f; }
bool validFov(float f) { return std::isfinite(f) && f > 0 && f < 180; }
}
float effectiveAdsTime(float gunSeconds, std::optional<float> cachedAds) {
    float value = cachedAds.value_or(gunSeconds);
    return std::isfinite(value) ? std::max(0.f, value) : .2f;
}
float advanceAimProgress(float progress, bool aiming, double elapsedMs, float adsSeconds) {
    progress = progressValue(progress);
    if (!std::isfinite(elapsedMs) || elapsedMs < 0) return progress;
    auto time = effectiveAdsTime(adsSeconds);
    if (time == 0) return aiming ? 1.f : 0.f;
    auto delta = static_cast<float>((elapsedMs + 1) / (time * 1000.0));
    return std::clamp(progress + (aiming ? delta : -delta), 0.f, 1.f);
}
bool fullyAimed(float progress) { return std::isfinite(progress) && progress >= 1.f; }
float magnificationToFov(float baseDegrees, float magnification) {
    if (!validFov(baseDegrees) || !std::isfinite(magnification) || magnification <= 0) return baseDegrees;
    return glm::degrees(2.f * std::atan(std::tan(glm::radians(baseDegrees) / 2.f) / magnification));
}
float aimingFov(float baseDegrees, float zoom, float progress) {
    if (!std::isfinite(zoom) || zoom <= 0) zoom = 1;
    return magnificationToFov(baseDegrees, 1.f + (zoom - 1.f) * progressValue(progress));
}
float aimingSensitivity(float baseDegrees, float zoom, float progress) {
    if (!validFov(baseDegrees)) return 1;
    auto current = aimingFov(baseDegrees, zoom, progress);
    return std::atan(std::tan(glm::radians(current) / 2.f) * 1.33f)
        / std::atan(std::tan(glm::radians(baseDegrees) / 2.f) * 1.33f);
}
float movementModifier(pack::MoveSpeed const& speed, bool aiming, bool reloading) {
    auto amount = reloading ? speed.reload : aiming ? speed.aim : speed.base;
    return std::isfinite(amount) ? std::max(amount, -1.f) : 0.f;
}
AimDynamics::AimDynamics(float f, float z, float r)
: k1(z / (std::numbers::pi_v<float> * f)),
  k2(1.f / std::pow(2.f * std::numbers::pi_v<float> * f, 2.f)),
  k3(r * z / (2.f * std::numbers::pi_v<float> * f)) {}
void AimDynamics::reset(float value) {
    x = y = std::isfinite(value) ? value : 0;
    velocity = 0;
    accumulated = 0;
}
float AimDynamics::update(float target, double elapsedSeconds) {
    if (!std::isfinite(target)) target = x;
    if (std::isfinite(elapsedSeconds) && elapsedSeconds > 0) accumulated += std::min(elapsedSeconds, .25);
    while (accumulated + 1e-12 >= .006) {
        constexpr float t = .05f;
        auto xd = (target - x) / t;
        auto next = y + t * velocity;
        velocity += t * (x + k3 * xd - y - k1 * velocity) / k2;
        x = target;
        y = next;
        accumulated -= .006;
    }
    if (!std::isfinite(y) || !std::isfinite(velocity)) reset(target);
    return y + .05f * velocity;
}
glm::mat4 blendAimViews(glm::mat4 const& idle, glm::mat4 const& iron, float progress) {
    float alpha = progressValue(progress);
    auto from = glm::normalize(glm::quat_cast(glm::mat3(idle)));
    auto to = glm::normalize(glm::quat_cast(glm::mat3(iron)));
    auto rotation = glm::normalize(glm::slerp(from, to, alpha));
    auto result = glm::mat4_cast(rotation);
    result[3] = glm::vec4(glm::mix(glm::vec3(idle[3]), glm::vec3(iron[3]), alpha), 1);
    return result;
}
glm::mat4 firstPersonAimTransform(glm::mat4 const& idle, glm::mat4 const& iron, float progress) {
    auto m = glm::translate(glm::mat4(1), {0, 1.5f, 0});
    m = glm::rotate(m, std::numbers::pi_v<float>, {0, 0, 1});
    m = glm::translate(m, {0, 1.5f, 0});
    m *= blendAimViews(idle, iron, progress);
    return glm::translate(m, {0, -1.5f, 0});
}
float projectionFov(glm::mat4 const& p) {
    if (!std::isfinite(p[1][1]) || p[1][1] <= 0 || std::abs(p[2][3] + 1.f) > .001f
        || std::abs(p[3][3]) > .001f) return 0;
    return glm::degrees(2.f * std::atan(1.f / p[1][1]));
}
glm::mat4 withModelFov(glm::mat4 p, float degrees) {
    auto base = projectionFov(p);
    if (!base || !validFov(degrees)) return p;
    float scale = std::tan(glm::radians(base) / 2.f) / std::tan(glm::radians(degrees) / 2.f);
    p[0][0] *= scale;
    p[1][1] *= scale;
    return p;
}
} // namespace tacz::model
