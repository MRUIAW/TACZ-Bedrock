#pragma once

#include "gun/DrawRules.h"
#include "gun/ShootRules.h"

namespace tacz::gun {

// LivingEntityShoot.java:228-262, translated onto our existing client timestamp
// timeline. Unlike receipt time, serverNow - clockOffset is comparable to lastShot.
inline int64_t syncedShootCooldownMs(bool hasGun, GunData const* data, std::optional<FireMode> mode,
    int64_t serverNow, std::optional<int64_t> clockOffset, std::optional<int64_t> lastShot) {
    if (!hasGun) return 0;
    if (!data) return -1;
    return serverCooldownMs(*data, mode, serverNow - clockOffset.value_or(0), lastShot);
}

// LivingEntityDrawGun.java:59-77, including future timestamps during put-away.
inline int64_t syncedDrawCooldownMs(bool hasGun, GunData const* data, DrawCooldown const& draw, int64_t now) {
    if (!hasGun) return 0;
    if (!data) return -1;
    return draw.remaining(now, animationMillis(data->drawTime));
}

// LivingEntityMelee.java:105-145. No melee producer/timestamp yet: its unset (-1)
// wall-clock timestamp gives zero for known gun data. Preserve no-gun/unknown
// getter results rather than publishing the schema's missing default as idle.
inline int64_t syncedMeleeCooldownMs(bool hasGun, GunData const* data) {
    if (!hasGun) return 0;
    return data ? 0 : -1;
}

} // namespace tacz::gun
