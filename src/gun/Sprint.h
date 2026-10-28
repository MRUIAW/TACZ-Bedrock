#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include "gun/Reload.h"

namespace tacz::gun {
// ShootKey.autoShoot: independent of ADS/display readiness, ammo and shot success.
inline bool shootStopsSprint(bool gameplay, bool mainHandGun, bool spectator, bool triggerHeld) {
    return gameplay && mainHandGun && !spectator && triggerHeld;
}

// Comparison-only actor identity, dimension and eligibility; never an engine pointer to dereference.
struct SprintIntentScope {
    uintptr_t actor = 0;
    int dimension = 0;
    bool usable = false;
    bool operator==(SprintIntentScope const&) const = default;
};

// Change-only transport/lifetime state, shared by the client publisher and server receiver.
// Observe BEFORE accepting a packet so old selection/life/dimension intent cannot persist.
struct ShootSprintIntent {
    bool held = false;
    std::optional<SprintIntentScope> scope;

    std::optional<bool> update(bool requested) {
        bool next = requested && scope && scope->usable;
        if (next == held) return std::nullopt;
        held = next;
        return held;
    }
    std::optional<bool> observe(SprintIntentScope next, bool selectionChanged = false) {
        bool reset = !scope || *scope != next || selectionChanged || !next.usable;
        scope = next;
        return reset ? update(false) : std::nullopt;
    }
};

inline bool blocksSprint(bool stopIntent, bool aiming, ReloadStateType reload) {
    return stopIntent || aiming || (isReloading(reload) && !isReloadFinishing(reload));
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
