#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <functional>
#include <optional>

#include "gun/Reload.h"
#include "gun/ShootRules.h"
#include "gun/BoltRules.h"
#include "pack/GunData.h"
#include "pack/ResourceId.h"

// The object gun logic scripts receive as `api`, after TACZ's ModernKineticGunScriptAPI.
// It works on a copy of the gun's state; the caller writes `ammo` back to the item afterwards.
// Reload and bolt are implemented; Lua shooting/heat follow later.
namespace tacz::lua {

class GunLogic;

struct ScriptApi {
    pack::ResourceId        gunId;
    pack::GunData const*    data = nullptr;
    gun::GunAmmo            ammo;
    int                     maxAmmo        = 0; // ammo_amount (extended magazines come with attachments)
    int                     magExtentLevel = 0;
    std::optional<pack::FireMode> fireMode;

    bool needCheckAmmo = true; // LivingEntityAmmoCheck.needCheckAmmo: not creative
    bool consumesAmmo  = true; // consumesAmmoOrNot: shooting uses ammo (creative too, TACZ default)

    gun::ReloadStateType reloadState  = gun::ReloadStateType::NotReloading;
    int64_t              reloadTimeMs = 0; // since the reload started
    int64_t              nowMs        = 0;
    float                aimingProgress = 0;
    bool                 isBolting = false;
    int64_t              boltNowMs = 0; // deterministic clock when boltClock is unset
    int64_t              boltTimestamp = 0; // caller reads back after script calls
    std::function<int64_t()> boltClock; // server wall clock, sampled on each getBoltTime

    // Removes up to n rounds of the gun's ammo from the shooter's inventory; returns how many.
    std::function<int(int)> extractAmmo;
    // Whether the shooter carries any of the gun's ammo.
    std::function<bool()> inventoryHasAmmo;

    // Set by GunLogic for the script data cache (ShooterDataHolder.scriptData).
    GunLogic* logic      = nullptr;
    int64_t   shooterKey = 0;

    [[nodiscard]] pack::Bolt bolt() const { return data->bolt.value_or(pack::Bolt::OpenBolt); }
    [[nodiscard]] bool useInventoryAmmo() const { return data->reload.type == pack::FeedType::Inventory; }

    [[nodiscard]] int  getNeededAmmoAmount() const { return maxAmmo - ammo.magazine; }
    [[nodiscard]] int  getAmmoAmount() const { return ammo.magazine; }
    [[nodiscard]] int  getMaxAmmoCount() const { return maxAmmo; }
    [[nodiscard]] int  getMagExtentLevel() const { return magExtentLevel; }
    [[nodiscard]] bool hasAmmoInBarrel() const { return bolt() != pack::Bolt::OpenBolt && ammo.barrel; }
    void               setAmmoInBarrel(bool value) { ammo.barrel = value; }

    // Returns what did not fit.
    int putAmmoInMagazine(int amount) {
        if (amount < 0) return 0;
        int total = ammo.magazine + amount;
        if (total > maxAmmo) {
            ammo.magazine = maxAmmo;
            return total - maxAmmo;
        }
        ammo.magazine = total;
        return 0;
    }

    // Returns how many were removed.
    int removeAmmoFromMagazine(int amount) {
        if (amount < 0) return 0;
        if (ammo.magazine < amount) {
            int removed   = ammo.magazine;
            ammo.magazine = 0;
            return removed;
        }
        ammo.magazine -= amount;
        return amount;
    }

    int consumeAmmoFromPlayer(int needed) {
        if (useInventoryAmmo() && !needCheckAmmo) return needed;
        return extractAmmo ? std::max(extractAmmo(needed), 0) : 0;
    }

    [[nodiscard]] bool hasAmmoToConsume() const {
        if (!needCheckAmmo) return true;
        return inventoryHasAmmo && inventoryHasAmmo();
    }

    // TACZ FireMode ordinals: AUTO 0, SEMI 1, BURST 2, UNKNOWN 3.
    [[nodiscard]] int getFireMode() const { return fireMode ? static_cast<int>(*fireMode) : 3; }
    // Java long subtraction wraps; preserve it without C++ signed overflow.
    static int64_t subtractTime(int64_t a, int64_t b) {
        return std::bit_cast<int64_t>(static_cast<uint64_t>(a) - static_cast<uint64_t>(b));
    }
    // ModernKineticGunScriptAPI.java:312-317,372-374. Inactive even inside start_bolt.
    [[nodiscard]] int64_t getBoltTime() const {
        return isBolting ? subtractTime(boltClock ? boltClock() : boltNowMs, boltTimestamp) : 0;
    }
    void adjustBoltTime(int64_t alpha) { boltTimestamp = subtractTime(boltTimestamp, alpha); }
};

// ModernKineticGunItem.java:340-359. Feed can precede OR follow the action time.
inline bool defaultTickBolt(ScriptApi& api) {
    auto action = gun::boltMillis(api.data->boltActionTime);
    auto feed = api.data->boltFeedTime == -1.f ? action : gun::boltMillis(api.data->boltFeedTime);
    if (api.getBoltTime() < feed) return true;
    if (!api.hasAmmoInBarrel()) {
        if (api.useInventoryAmmo()) {
            if (api.consumeAmmoFromPlayer(1) == 1) api.setAmmoInBarrel(true);
        } else if (api.removeAmmoFromMagazine(1) != 0) api.setAmmoInBarrel(true);
    }
    return api.getBoltTime() < action;
}

// ModernKineticGunItem.defaultTickReload + defaultReloadFinishing, for guns without a
// tick_reload script function.
inline gun::ReloadState defaultTickReload(ScriptApi& api) {
    using gun::ReloadStateType;
    auto const& reload = api.data->reload;
    auto        old    = api.reloadState;
    auto        ms     = [](float seconds) { return static_cast<int64_t>(seconds * 1000); };

    gun::ReloadState next;
    auto stage = [&](float feed, float finish, ReloadStateType feeding, ReloadStateType finishing) {
        if (api.reloadTimeMs < ms(feed)) next = {feeding, ms(feed) - api.reloadTimeMs};
        else if (api.reloadTimeMs < ms(finish)) next = {finishing, ms(finish) - api.reloadTimeMs};
        else next = {};
    };
    if (gun::isReloadingEmpty(old)) {
        stage(reload.feed.empty, reload.cooldown.empty, ReloadStateType::EmptyFeeding, ReloadStateType::EmptyFinishing);
    } else if (gun::isReloadingTactical(old)) {
        stage(reload.feed.tactical, reload.cooldown.tactical, ReloadStateType::TacticalFeeding, ReloadStateType::TacticalFinishing);
    }

    bool finishedFeeding = (old == ReloadStateType::EmptyFeeding || old == ReloadStateType::TacticalFeeding) && next.type != old;
    if (finishedFeeding) {
        bool tactical = old == ReloadStateType::TacticalFeeding;
        int  needed   = api.getNeededAmmoAmount();
        bool consume  = api.needCheckAmmo && !reload.infinite;
        switch (reload.type.value_or(pack::FeedType::Magazine)) {
        case pack::FeedType::Magazine:
            api.putAmmoInMagazine(consume ? api.consumeAmmoFromPlayer(needed) : needed);
            break;
        case pack::FeedType::Fuel:
            api.putAmmoInMagazine(consume ? needed * api.consumeAmmoFromPlayer(1) : needed);
            break;
        default:
            break; // TACZ: not implemented
        }
        if (!tactical && (api.bolt() == pack::Bolt::ManualAction || api.bolt() == pack::Bolt::ClosedBolt)) {
            if (api.removeAmmoFromMagazine(1) != 0) api.setAmmoInBarrel(true);
        }
    }
    return next;
}

} // namespace tacz::lua
