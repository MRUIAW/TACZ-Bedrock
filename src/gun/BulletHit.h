#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

#include <glm/glm.hpp>

// Bullet-vs-entity geometry from TACZ (util/HitboxHelper.getFixedBoundingBox,
// util/EntityUtil.getHitResult), kept free of game types so it can be unit tested.
namespace tacz::gun {

struct Box {
    glm::dvec3 min{0};
    glm::dvec3 max{0};

    // Minecraft AABB.expandTowards: grows only on the side the vector points to.
    [[nodiscard]] Box expandTowards(glm::dvec3 v) const {
        Box b = *this;
        for (int i = 0; i < 3; ++i) {
            if (v[i] < 0) b.min[i] += v[i];
            else b.max[i] += v[i];
        }
        return b;
    }

    [[nodiscard]] Box move(glm::dvec3 v) const { return {min + v, max + v}; }

    [[nodiscard]] bool contains(glm::dvec3 p) const {
        return glm::all(glm::greaterThanEqual(p, min)) && glm::all(glm::lessThanEqual(p, max));
    }
};

// What getFixedBoundingBox needs to know about the target.
struct HitboxTarget {
    Box        box;              // the entity's collision box
    glm::dvec3 velocity{0};      // position change over the last tick
    bool       isPlayer   = false;
    bool       isCrouching = false;
    bool       isRiding   = false;
};

// TACZ config defaults (OtherConfig.SERVER_HITBOX_OFFSET).
inline constexpr double PlayerHitboxOffset = 3.0;

// getFixedBoundingBox without the player-vs-player latency rewind: the box grows a little for
// standing players, stretches along the target's motion, and is shifted back along it to match
// where clients draw moving entities.
inline Box fixedHitbox(HitboxTarget const& t) {
    Box  box          = t.box;
    auto vel          = t.velocity;
    double expandHeight = t.isPlayer && !t.isCrouching ? 0.0625 : 0.0;
    box               = box.expandTowards({0, expandHeight, 0});
    box               = box.expandTowards(vel);
    if (t.isPlayer) {
        if (t.isRiding) box = box.move(vel * (PlayerHitboxOffset / 2));
        box = box.move(vel * PlayerHitboxOffset);
    }
    if (t.isRiding) box = box.move(vel * -2.5);
    return box.move(vel * -5.0);
}

// Minecraft AABB.clip: the point where the segment from -> to enters the box. A segment that
// starts inside the box does not hit it.
inline std::optional<glm::dvec3> clip(Box const& box, glm::dvec3 from, glm::dvec3 to) {
    glm::dvec3 d     = to - from;
    double     enter = 0.0;
    double     exit  = 1.0;
    bool       crossed = false;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-12) {
            if (from[i] < box.min[i] || from[i] > box.max[i]) return std::nullopt;
            continue;
        }
        double t1 = (box.min[i] - from[i]) / d[i];
        double t2 = (box.max[i] - from[i]) / d[i];
        if (t1 > t2) std::swap(t1, t2);
        if (t1 > enter) {
            enter   = t1;
            crossed = true;
        }
        exit = std::min(exit, t2);
        if (enter > exit) return std::nullopt;
    }
    if (!crossed) return std::nullopt; // started inside, or never reached a face
    return from + d * enter;
}

// TACZ's default headshot rule: the hit lies within 0.25 blocks of the target's eye height.
inline bool isHeadshot(double hitY, double eyeY) { return hitY > eyeY - 0.25 && hitY < eyeY + 0.25; }

} // namespace tacz::gun
