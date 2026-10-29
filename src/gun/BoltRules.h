#pragma once

#include <cmath>
#include <limits>
#include "gun/Reload.h"
#include "gun/ShootRules.h"

namespace tacz::gun {

// Java float multiplication followed by narrowing conversion to long, including
// saturation/NaN semantics. Do not clamp negative feed times or collapse feed/action.
inline int64_t boltMillis(float seconds) {
    float ms = seconds * 1000.f;
    if (std::isnan(ms)) return 0;
    if (ms >= static_cast<float>(std::numeric_limits<int64_t>::max())) return std::numeric_limits<int64_t>::max();
    if (ms <= static_cast<float>(std::numeric_limits<int64_t>::min())) return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(ms);
}

struct BoltCheck {
    bool hasGun = true, knownData = true, hasDisplay = true;
    bool clientLocked = false, localBolting = false, serverBolting = false;
    int64_t shootCooldown = 0, drawCooldown = 0;
    ReloadStateType reload = ReloadStateType::NotReloading;
    std::optional<Bolt> type = Bolt::ManualAction;
    GunAmmo ammo;
    bool inventoryFed = false, inventoryHasAmmo = false, needCheckAmmo = true;
    bool scriptedHooks = false; // TEMP 0.4.8 staging; removed in 0.4.9
};
inline char const* boltAmmoCheck(BoltCheck const& c) {
    if (c.type != Bolt::ManualAction) return "NOT_MANUAL_ACTION";
    if (c.ammo.barrel) return "CHAMBERED";
    if (c.inventoryFed ? (c.needCheckAmmo && !c.inventoryHasAmmo) : c.ammo.magazine < 1) return "NO_AMMO";
    return nullptr;
}
// LivingEntityBolt.java:27-78. No extra death/sprint/pending-burst check.
inline char const* serverBoltRefusal(BoltCheck const& c) {
    if (!c.hasGun) return "NOT_GUN";
    if (!c.knownData) return "ID_NOT_EXIST";
    if (c.shootCooldown != 0) return "COOL_DOWN";
    if (isReloading(c.reload)) return "IS_RELOADING";
    if (c.drawCooldown != 0) return "IS_DRAWING";
    if (c.serverBolting) return "IS_BOLTING";
    if (auto reason = boltAmmoCheck(c)) return reason;
    if (c.scriptedHooks) return "SCRIPT_BOLT_DEFERRED";
    return nullptr;
}
// LocalPlayerBolt.java:26-78 deliberately tests the general lock and local latch,
// not synced cooldowns/reload/is_bolting individually.
inline char const* clientBoltRefusal(BoltCheck const& c) {
    if (c.clientLocked) return "STATE_LOCKED";
    if (c.localBolting) return "LOCAL_BOLTING";
    if (!c.hasGun) return "NOT_GUN";
    if (!c.knownData) return "ID_NOT_EXIST";
    if (!c.hasDisplay) return "NO_DISPLAY";
    if (auto reason = boltAmmoCheck(c)) return reason;
    if (c.scriptedHooks) return "SCRIPT_BOLT_DEFERRED";
    return nullptr;
}
// Shared shoot/reload authoritative exclusion, not an animation completion test.
inline bool boltBlocksAction(bool isBolting) { return isBolting; }

struct ClientBoltLatch {
    bool isBolting = false;
    void reset() { isBolting = false; }
    // LocalPlayerBolt.java:81-94: timeout/refusal does NOT clear this latch.
    void observeItem(bool hasGun, bool chambered) { if (!hasGun || chambered) isBolting = false; }
};

} // namespace tacz::gun
