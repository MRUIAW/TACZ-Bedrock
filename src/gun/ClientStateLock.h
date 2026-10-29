#pragma once

#include "gun/SyncedShooterData.h"

namespace tacz::gun {

// LocalPlayerDataHolder.java:81-118. Enum counterparts of Java's predicates;
// no engine pointers, local countdowns, packet acknowledgements or bolt latch.
enum class LockCondition { None, Shoot, Draw, Reload, Bolt, Melee };

struct ClientStateLock {
    bool clientStateLock = false;
    LockCondition lockedCondition = LockCondition::None;
    int64_t lockTimestamp = -1;

    void lockState(LockCondition condition, int64_t now) {
        clientStateLock = true;
        lockedCondition = condition;
        lockTimestamp = now;
    }
    bool conditionMet(sync::Snapshot const& state) const {
        switch (lockedCondition) {
        case LockCondition::None: return true;
        case LockCondition::Shoot: return state.shootCoolDown() > 0;
        case LockCondition::Draw: return state.drawCoolDown() > 0;
        case LockCondition::Reload: return isReloading(state.reload().type);
        case LockCondition::Bolt: return state.isBolting();
        case LockCondition::Melee: return state.meleeCoolDown() > 0;
        }
        return false;
    }
    // Return true only when an unsatisfied condition expired, for the temporary
    // acceptance counter. Timeout does NOT bypass active authoritative actions.
    bool tickStateLock(int64_t now, sync::Snapshot const& state) {
        bool met = conditionMet(state);
        bool pending = lockedCondition != LockCondition::None;
        auto elapsed = now - lockTimestamp;
        if (elapsed < 250 && pending && !met) return false;
        bool timedOut = pending && !met && elapsed >= 250;
        lockedCondition = LockCondition::None;
        if (isReloading(state.reload().type) || state.shootCoolDown() > 0 || state.drawCoolDown() > 0
            || state.isBolting() || state.meleeCoolDown() > 0) return timedOut;
        clientStateLock = false; // tick never acquires a lock
        return timedOut;
    }
    // LocalPlayerShoot.java:136: a Shoot lock is NOT the cadence clock.
    bool pendingNonShoot() const {
        return clientStateLock && lockedCondition != LockCondition::None && lockedCondition != LockCondition::Shoot;
    }
    void reset() { *this = {}; }
};

// LocalPlayerShoot.java:189-204, including an early-fed but still-active bolt.
inline bool syncedShootPrecheck(sync::Snapshot const& state) {
    return !isReloading(state.reload().type) && state.drawCoolDown() == 0 && !state.isBolting() && state.meleeCoolDown() == 0;
}

} // namespace tacz::gun
