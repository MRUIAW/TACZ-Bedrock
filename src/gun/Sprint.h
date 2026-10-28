#pragma once

#include <algorithm>
#include <optional>

namespace tacz::gun {
// ShootKey.autoShoot: independent of ADS/display readiness, ammo and shot success.
inline bool shootStopsSprint(bool gameplay, bool mainHandGun, bool spectator, bool triggerHeld) {
    return gameplay && mainHandGun && !spectator && triggerHeld;
}

// LivingEntityAim.tickSprint. The adapter supplies real elapsed seconds, not a
// fixed tick duration. Missing/unknown held gun clears the shooter timer.
inline float advanceSprintTime(float current, float elapsedSeconds, std::optional<float> gunSprintTime,
    bool sprinting, bool crouching) {
    if (!gunSprintTime) return 0;
    if (sprinting && !crouching) return std::min(current + elapsedSeconds, *gunSprintTime);
    return std::max(current - elapsedSeconds, 0.f);
}

inline bool sprintBlocksShot(float sprintTime) { return sprintTime > 0; }
} // namespace tacz::gun
