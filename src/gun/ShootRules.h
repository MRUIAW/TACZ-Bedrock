#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#include "gun/GunStats.h"
#include "pack/GunData.h"

// Shooting rules from TACZ, free of game types: fire mode cycling, trigger cooldowns
// (LivingEntityShoot / LocalPlayerShoot), chambering and ammo use per bolt type
// (ModernKineticGunScriptAPI.reduceAmmoOnce), inaccuracy and bullet spread.
namespace tacz::gun {

using pack::Bolt;
using pack::FireMode;
using pack::InaccuracyType;

// AbstractGunItem.fireSelect: the next mode in the gun's list; an unknown mode goes to the first.
inline std::optional<FireMode>
nextFireMode(std::vector<std::optional<FireMode>> const& modes, std::optional<FireMode> current) {
    if (modes.empty()) return current;
    auto   it    = std::find(modes.begin(), modes.end(), current);
    size_t index = it == modes.end() ? 0 : (static_cast<size_t>(it - modes.begin()) + 1) % modes.size();
    return modes[index];
}

// TACZ AmmoConfig.GLOBAL_BULLET_SPEED_MODIFIER default: every bullet flies at twice its pack speed.
inline constexpr float GlobalBulletSpeedModifier = 2.0f;

// AmmoSpeedModifier.initCache + ModernKineticGunScriptAPI.shootOnce: (speed + fire mode adjust)
// * global modifier, then / 20 for blocks per tick.
inline double bulletSpeedPerTick(GunData const& data, std::optional<FireMode> mode) {
    float speed = data.bullet.speed;
    if (auto const* adjust = data.fireModeAdjustFor(mode)) speed += adjust->speed;
    return std::max(static_cast<double>(speed * GlobalBulletSpeedModifier) / 20.0, 0.0);
}

// ShootKey.autoShoot: holding the trigger keeps firing in AUTO, and in BURST with continuous_shoot.
inline bool firesContinuously(GunData const& data, std::optional<FireMode> mode) {
    return mode == FireMode::Auto || (mode == FireMode::Burst && data.burst.continuousShoot);
}

// Minimum time between trigger pulls, ms: burst min_interval, otherwise the shoot interval.
inline int64_t triggerIntervalMs(GunData const& data, std::optional<FireMode> mode) {
    if (mode == FireMode::Burst) return static_cast<int64_t>(data.burst.minInterval * 1000.0);
    return shootIntervalMs(data, mode);
}

// LivingEntityShoot.getShootCoolDown: remaining cooldown at `now` (client timestamps), less
// TACZ's 5 ms latency window. No previous shot means no cooldown.
inline int64_t serverCooldownMs(GunData const& data, std::optional<FireMode> mode, int64_t now, std::optional<int64_t> lastShot) {
    if (!lastShot) return 0;
    return std::max<int64_t>(triggerIntervalMs(data, mode) - (now - *lastShot) - 5, 0);
}

// LocalPlayerShoot.getCoolDown: the client's own view, without the window.
inline int64_t clientCooldownMs(GunData const& data, std::optional<FireMode> mode, int64_t now, std::optional<int64_t> lastShot) {
    if (!lastShot) return 0;
    return std::max<int64_t>(triggerIntervalMs(data, mode) - (now - *lastShot), 0);
}

// Magazine-fed ammo state as stored on the gun item.
struct GunAmmo {
    int  magazine = 0;
    bool barrel   = false;

    auto operator<=>(GunAmmo const&) const = default;
};

// Rounds that can be fired: the chambered round only counts for closed and manual bolts.
inline int availableRounds(Bolt bolt, GunAmmo const& ammo) {
    return ammo.magazine + (ammo.barrel && bolt != Bolt::OpenBolt ? 1 : 0);
}

enum class ShotCheck { Ok, NoAmmo, NeedBolt };

// The ammo checks of LivingEntityShoot.shoot, including chambering a round from the magazine
// when a closed-bolt gun has an empty chamber.
inline ShotCheck prepareShot(Bolt bolt, GunAmmo& ammo) {
    bool chambered = ammo.barrel && bolt != Bolt::OpenBolt;
    if (availableRounds(bolt, ammo) < 1) return ShotCheck::NoAmmo;
    if (bolt == Bolt::ManualAction && !chambered) return ShotCheck::NeedBolt;
    if (bolt == Bolt::ClosedBolt && !chambered) {
        ammo.magazine -= 1;
        ammo.barrel = true;
    }
    return ShotCheck::Ok;
}

// ModernKineticGunScriptAPI.reduceAmmoOnce for magazine-fed guns (inventory-fed guns are not
// supported yet). Returns false when there is nothing to fire.
inline bool reduceAmmoOnce(Bolt bolt, GunAmmo& ammo) {
    bool chambered = ammo.barrel && bolt != Bolt::OpenBolt;
    bool noAmmo    = ammo.magazine < 1;
    switch (bolt) {
    case Bolt::ManualAction:
        if (!chambered) return false;
        ammo.barrel = false;
        return true;
    case Bolt::ClosedBolt:
        if (!noAmmo) {
            ammo.magazine -= 1;
            return true;
        }
        if (!chambered) return false;
        ammo.barrel = false;
        return true;
    case Bolt::OpenBolt:
        if (noAmmo) return false;
        ammo.magazine -= 1;
        return true;
    }
    return false;
}

// InaccuracyType.getInaccuracyType. Bedrock has no prone pose, so LIE never applies.
inline InaccuracyType inaccuracyType(bool aiming, bool sneaking, double speedPerTick) {
    if (aiming) return InaccuracyType::Aim;
    if (sneaking) return InaccuracyType::Sneak;
    if (speedPerTick > 0.05) return InaccuracyType::Move;
    return InaccuracyType::Stand;
}

// InaccuracyModifier.initCache: the gun's value for the state plus the fire mode's aim or
// other addend, never below 0.
inline float inaccuracy(GunData const& data, std::optional<FireMode> mode, InaccuracyType type) {
    float base = 0;
    if (data.inaccuracy) {
        if (auto it = data.inaccuracy->find(type); it != data.inaccuracy->end()) base = it->second;
    }
    float addend = 0;
    if (auto const* adjust = data.fireModeAdjustFor(mode)) {
        addend = type == InaccuracyType::Aim ? adjust->aimInaccuracy : adjust->otherInaccuracy;
    }
    return std::max(base + addend, 0.0f);
}

// Vanilla Projectile.shoot, which TACZ's default doBulletSpread calls: the unit direction gets a
// triangular jitter of 0.0172275 * inaccuracy on each axis and is then scaled by the speed,
// without renormalising.
template <class Rng>
glm::dvec3 spreadVelocity(glm::dvec3 direction, double speedPerTick, float inaccuracyValue, Rng& rng) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    double     spread = 0.0172275 * static_cast<double>(inaccuracyValue);
    double     len    = glm::length(direction);
    glm::dvec3 dir    = len > 0 ? direction / len : glm::dvec3{0, 0, 1};
    for (int i = 0; i < 3; ++i) dir[i] += spread * (unit(rng) - unit(rng));
    return dir * speedPerTick;
}

} // namespace tacz::gun
