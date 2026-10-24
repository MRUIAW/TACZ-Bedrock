#include "model/CameraRecoil.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tacz::model {
RecoilSpline::RecoilSpline(std::vector<double> timesMs, std::vector<double> values) : times(std::move(timesMs)) {
    auto count = times.size();
    if (count < 3 || count != values.size()) throw std::invalid_argument("natural spline requires >=3 equal-sized knots/values");
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(times[i]) || !std::isfinite(values[i]) || (i && times[i] <= times[i-1]))
            throw std::invalid_argument("nonfinite or non-increasing recoil knots");
    }
    // Solve the natural-spline tridiagonal system for second derivatives. The endpoint
    // equations are M[0]=M[n]=0; use double precision like PolynomialSplineFunction.
    std::vector<double> diagonal(count, 1), upper(count), rhs(count), second(count);
    for (size_t i = 1; i + 1 < count; ++i) {
        double left = times[i] - times[i-1], right = times[i+1] - times[i];
        diagonal[i] = 2 * (left + right);
        upper[i] = right;
        rhs[i] = 6 * ((values[i+1] - values[i]) / right - (values[i] - values[i-1]) / left);
        auto factor = left / diagonal[i-1];
        diagonal[i] -= factor * upper[i-1];
        rhs[i] -= factor * rhs[i-1];
    }
    for (size_t i = count - 2; i > 0; --i) second[i] = (rhs[i] - upper[i] * second[i+1]) / diagonal[i];
    segments.reserve(count - 1);
    for (size_t i = 0; i + 1 < count; ++i) {
        double h = times[i+1] - times[i];
        Polynomial p{values[i], (values[i+1] - values[i]) / h - h * (2 * second[i] + second[i+1]) / 6,
            second[i] / 2, (second[i+1] - second[i]) / (6 * h)};
        if (!std::isfinite(p.b) || !std::isfinite(p.c) || !std::isfinite(p.d))
            throw std::invalid_argument("nonfinite recoil spline coefficient");
        segments.push_back(p);
    }
}
std::optional<double> RecoilSpline::evaluate(double elapsedMs) const {
    if (!std::isfinite(elapsedMs) || elapsedMs < times.front() || elapsedMs > times.back()) return {};
    auto index = static_cast<size_t>(std::upper_bound(times.begin(), times.end(), elapsedMs) - times.begin() - 1);
    index = std::min(index, segments.size() - 1);
    auto t = elapsedMs - times[index];
    auto const& p = segments[index];
    return ((p.d * t + p.c) * t + p.b) * t + p.a;
}
RecoilCurve buildRecoilCurve(std::span<pack::RecoilKeyFrame const> frames, float modifier, std::mt19937& random) {
    if (frames.empty()) return {};
    if (frames.size() > 4096) return {{}, "recoil axis exceeds 4096 keyframes"};
    if (!std::isfinite(modifier)) return {{}, "nonfinite recoil modifier"};
    auto sorted = std::vector<pack::RecoilKeyFrame>(frames.begin(), frames.end());
    for (auto const& f : sorted) {
        if (!std::isfinite(f.time) || f.time < 0 || f.value.size() != 2 || !std::isfinite(f.value[0])
            || !std::isfinite(f.value[1]) || f.value[0] > f.value[1]) return {{}, "invalid recoil time/range"};
    }
    // CommonGunIndex.checkRecoil sorts object keyframes before GunRecoil samples them.
    std::stable_sort(sorted.begin(), sorted.end(), [](auto const& a, auto const& b) { return a.time < b.time; });
    std::vector<double> times{0}, values{0};
    for (auto const& f : sorted) {
        // Java time is a float; preserve its *1000 +30 float arithmetic before widening.
        times.push_back(static_cast<double>(f.time * 1000.f + 30.f));
        float width = f.value[1] - f.value[0];
        double u = std::generate_canonical<double, 53>(random); // [0,1), sampled anew per shot/key
        values.push_back((f.value[0] + u * width) * modifier);
    }
    try { return {RecoilSpline{std::move(times), std::move(values)}, {}}; }
    catch (std::invalid_argument const& e) { return {{}, e.what()}; }
}
float cameraRecoilModifier(float aimProgress, float zoom, float crawl, float attachment) {
    float aim = std::isfinite(aimProgress) ? std::clamp(aimProgress, 0.f, 1.f) : 0;
    if (!std::isfinite(zoom) || zoom <= 0) zoom = 1;
    if (!std::isfinite(crawl)) crawl = 1;
    if (!std::isfinite(attachment)) attachment = 1;
    return (1 - aim + aim / std::min(std::sqrt(zoom), 1.5f)) * crawl * attachment;
}
void CameraRecoil::shot(std::optional<RecoilSpline> p, std::optional<RecoilSpline> y, double nowMs) {
    clear();
    if (!std::isfinite(nowMs)) return;
    pitch = std::move(p); yaw = std::move(y);
    startedMs = sampledMs = nowMs;
}
RecoilDelta CameraRecoil::update(double nowMs) {
    RecoilDelta result;
    if (!std::isfinite(nowMs) || nowMs < sampledMs) return result;
    sampledMs = nowMs;
    auto sample = [&](std::optional<RecoilSpline>& curve, double& previous) {
        if (!curve) return 0.0;
        auto value = curve->evaluate(nowMs - startedMs);
        if (!value) {
            // Match isValidPoint: do NOT force the final sample after a missed endpoint,
            // add a recovery spring, or extrapolate a tail beyond the data's domain.
            if (nowMs - startedMs > curve->endMs()) curve.reset();
            return 0.0;
        }
        auto delta = -(*value - previous);
        previous = *value;
        return delta;
    };
    result.pitch = sample(pitch, previousPitch);
    result.yaw = sample(yaw, previousYaw);
    return result;
}
void CameraRecoil::clear() {
    pitch.reset(); yaw.reset();
    startedMs = sampledMs = previousPitch = previousYaw = 0;
}
} // namespace tacz::model
