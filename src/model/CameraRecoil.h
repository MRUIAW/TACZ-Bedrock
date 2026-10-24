#pragma once

#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>
#include "pack/GunData.h"

namespace tacz::model {

// Natural cubic spline (zero endpoint second derivatives), the mathematical interpolant
// used by Commons Math 3 SplineInterpolator. Not a Bedrock animation Catmull-Rom curve.
class RecoilSpline {
public:
    RecoilSpline(std::vector<double> timesMs, std::vector<double> values);
    std::optional<double> evaluate(double elapsedMs) const; // inclusive domain, no extrapolation
    double endMs() const { return times.back(); }
    std::span<double const> knots() const { return times; }
private:
    struct Polynomial { double a, b, c, d; };
    std::vector<double> times;
    std::vector<Polynomial> segments;
};

struct RecoilCurve {
    std::optional<RecoilSpline> spline;
    std::string error; // missing axis is normal; malformed axis has a reason
};
RecoilCurve buildRecoilCurve(std::span<pack::RecoilKeyFrame const> frames, float modifier, std::mt19937& random);
float cameraRecoilModifier(float aimProgress, float zoom, float crawl = 1.f, float attachment = 1.f);

struct RecoilDelta {
    double pitch = 0, yaw = 0; // actual look delta: -(curve value - previous sample), degrees
};

// CameraSetupEvent has ONE current pair of curves. A new shot replaces the previous pair
// and zeroes previous samples; it does NOT undo the rotation already applied or sum tails.
class CameraRecoil {
public:
    void shot(std::optional<RecoilSpline> pitch, std::optional<RecoilSpline> yaw, double nowMs);
    RecoilDelta update(double nowMs);
    void clear(); // discard pending recoil, never synthesize a return-to-original-angle
    bool active() const { return pitch.has_value() || yaw.has_value(); }
private:
    std::optional<RecoilSpline> pitch, yaw;
    double startedMs = 0, sampledMs = 0, previousPitch = 0, previousYaw = 0;
};
} // namespace tacz::model
