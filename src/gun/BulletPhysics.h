#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/glm.hpp>

#include "pack/GunData.h"

// Bullet motion as in TACZ's EntityKineticBullet: speed is in blocks per second and becomes
// speed / 20 blocks per tick; each tick the bullet moves by its velocity, then the velocity
// is scaled by (1 - friction) and loses `gravity` on y. Water drag is not modelled yet.
namespace tacz::gun {

struct BulletMotion {
    glm::dvec3 pos{0};
    glm::dvec3 vel{0}; // blocks per tick
    float      gravity  = 0;
    float      friction = 0;
    int        life     = 1; // ticks, TACZ: clamp(life_seconds * 20, 1, INT_MAX)
    int        age      = 0; // ticks lived (TACZ tickCount)

    // Velocity at launch from bullet data: normalised direction * speed / 20.
    static BulletMotion launch(pack::BulletData const& bullet, glm::dvec3 origin, glm::dvec3 direction) {
        double     len = glm::length(direction);
        glm::dvec3 dir = len > 0 ? direction / len : glm::dvec3{0, 0, 1};
        return launchWithVelocity(bullet, origin, dir * static_cast<double>(std::max(bullet.speed / 20.0f, 0.0f)));
    }

    // Launch with an already computed velocity (blocks per tick), e.g. after spread.
    static BulletMotion launchWithVelocity(pack::BulletData const& bullet, glm::dvec3 origin, glm::dvec3 velocity) {
        BulletMotion m;
        m.pos      = origin;
        m.vel      = velocity;
        m.gravity  = std::max(bullet.gravity, 0.0f);
        m.friction = std::max(bullet.friction, 0.0f);
        m.life     = std::max(static_cast<int>(bullet.life * 20), 1);
        return m;
    }

    // Where the bullet would be after this tick's move (the segment to ray-trace).
    [[nodiscard]] glm::dvec3 next() const { return pos + vel; }

    // Commits one tick: move, then drag and gravity. Returns false once the bullet expires
    // (TACZ discards when tickCount >= life - 1, after moving).
    bool step() {
        ++age;
        pos = next();
        vel *= 1.0 - friction;
        vel.y -= gravity;
        return age < life - 1;
    }

    // Bedrock rotation for an entity flying along `vel`: (pitch, yaw) in degrees,
    // pitch positive looking down, yaw 0 facing +z and 90 facing -x.
    [[nodiscard]] glm::vec2 rotation() const {
        double horizontal = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        double toDeg      = 180.0 / std::numbers::pi;
        return {
            static_cast<float>(-std::atan2(vel.y, horizontal) * toDeg),
            static_cast<float>(std::atan2(-vel.x, vel.z) * toDeg),
        };
    }
};

} // namespace tacz::gun
