#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include "gun/BulletHit.h"

namespace tacz::debug::hit {

inline constexpr unsigned LineLimit = 64;
inline constexpr unsigned ShotLimit = 16;
inline constexpr uint64_t TickLimit = 400;

// Observation identity only; never used to aim, rewind or advance a real bullet.
struct Tag {
    uint64_t generation = 0;
    unsigned shot = 0;
    uint64_t tick = 0;
    int64_t timestamp = 0;
};

class Budget {
public:
    void arm(unsigned shots) { remaining = std::min(shots, ShotLimit); lines = 0; ++generation; nextShot = 0; }
    std::optional<Tag> trigger(uint64_t tick, int64_t timestamp) {
        if (!remaining || lines >= LineLimit) return {};
        --remaining;
        return Tag{generation, ++nextShot, tick, timestamp};
    }
    bool take(Tag const& tag) {
        if (!accepts(tag)) return false;
        ++lines; return true;
    }
    bool accepts(Tag const& tag) const { return tag.generation && tag.generation == generation && lines < LineLimit; }
    bool available() const { return generation && lines < LineLimit && (remaining || nextShot); }
    unsigned used() const { return lines; }
private:
    uint64_t generation = 0;
    unsigned remaining = 0, nextShot = 0, lines = 0;
};

// Fixed-phase snapshots, not native physics velocity. Missing/nonconsecutive
// history stays unknown; sampling a candidate never manufactures a zero delta.
class PositionHistory {
public:
    using Samples = std::map<int64_t, glm::dvec3>;
    void sample(uint64_t tick, Samples positions) {
        previous = std::move(current); current = std::move(positions);
        previousTick = currentTick; currentTick = tick;
    }
    std::optional<glm::dvec3> previousPosition(int64_t id, uint64_t tick) const {
        if (currentTick != tick || previousTick + 1 != tick) return {};
        auto found = previous.find(id);
        if (found == previous.end() || !current.contains(id)) return {};
        return found->second;
    }
    void clear() { previous.clear(); current.clear(); currentTick = previousTick = 0; }
private:
    Samples previous, current;
    uint64_t previousTick = 0, currentTick = 0;
};

inline glm::dvec3 midpointEye(glm::dvec3 head, glm::dvec3 current, glm::dvec3 previous) {
    return head + (previous - current) * 0.5;
}

// Exact minimum Euclidean distance from a finite segment to an axis-aligned
// solid box. Split t in [0,1] at slab boundaries; each interval is quadratic.
// Independent from gun::clip's Java "starts inside => no entry hit" semantics.
inline double segmentBoxDistance(gun::Box const& box, glm::dvec3 from, glm::dvec3 to) {
    auto d = to - from;
    std::array<double, 8> cuts{0, 1};
    size_t count = 2;
    for (int axis = 0; axis < 3; ++axis) if (d[axis] != 0) {
        for (double face : {box.min[axis], box.max[axis]}) {
            double t = (face - from[axis]) / d[axis];
            if (t > 0 && t < 1) cuts[count++] = t;
        }
    }
    std::sort(cuts.begin(), cuts.begin() + count);
    auto squared = [&](double t) {
        auto p = from + d * t;
        auto outside = glm::max(glm::max(box.min - p, p - box.max), glm::dvec3{0});
        return glm::dot(outside, outside);
    };
    double best = std::numeric_limits<double>::infinity();
    for (size_t i = 1; i < count; ++i) {
        double lo = cuts[i - 1], hi = cuts[i], mid = (lo + hi) * 0.5;
        double a = 0, b = 0;
        for (int axis = 0; axis < 3; ++axis) {
            double p = from[axis] + d[axis] * mid;
            if (p >= box.min[axis] && p <= box.max[axis]) continue;
            double edge = p < box.min[axis] ? box.min[axis] : box.max[axis];
            a += d[axis] * d[axis]; b += d[axis] * (from[axis] - edge);
        }
        best = std::min({best, squared(lo), squared(hi), squared(a > 0 ? std::clamp(-b / a, lo, hi) : lo)});
    }
    return std::sqrt(best);
}

} // namespace tacz::debug::hit
