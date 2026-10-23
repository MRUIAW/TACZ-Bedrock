#pragma once

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <optional>
#include <vector>

#include "pack/GunData.h"

// Per-shot gun values derived from GunData, following TACZ's attachment-free cache
// initialisers (resource/modifier/custom/*Modifier.initCache) and EntityKineticBullet.
// Attachment modifiers and Lua property overrides are applied on top of these later.
namespace tacz::gun {

using pack::FireMode;
using pack::GunData;

// Server config multipliers (TACZ SyncConfig). All default to 1.
struct DamageConfig {
    float damageBase      = 1;
    float headShotBase    = 1;
    float armorIgnoreBase = 1;
};

// GunData.getRoundsPerMinute(FireMode): base rpm + fire mode adjust; invalid -> 300.
inline int roundsPerMinute(GunData const& data, std::optional<FireMode> mode) {
    int rpm = data.rpm;
    if (auto const* adjust = data.fireModeAdjustFor(mode)) rpm += adjust->rpm;
    return rpm <= 0 ? 300 : rpm;
}

// GunData.getShootInterval without heat: 60000 / clamp(rpm, 1, 1200) ms.
inline int64_t shootIntervalMs(GunData const& data, std::optional<FireMode> mode) {
    return 60'000 / std::clamp(roundsPerMinute(data, mode), 1, 1200);
}

// GunData.getBurstShootInterval.
inline int64_t burstShootIntervalMs(GunData const& data) {
    return data.burst.bpm <= 0 ? 300 : 60'000 / data.burst.bpm;
}

// DamageModifier.initCache: the distance/damage table with fire mode adjust and config applied.
inline std::vector<pack::DistanceDamagePair>
damageTable(GunData const& data, std::optional<FireMode> mode, DamageConfig const& config = {}) {
    float adjust = 0;
    if (auto const* a = data.fireModeAdjustFor(mode)) adjust = a->damage;
    std::vector<pack::DistanceDamagePair> table;
    auto const& extra = data.bullet.extraDamage;
    // TACZ treats an absent damage_adjust list as "no table" only when extra_damage is null;
    // an empty list yields an empty table (every hit does 0 damage), and we keep that.
    if (extra) {
        for (auto const& pair : extra->damageAdjust) {
            table.push_back({pair.distance, (pair.damage + adjust) * config.damageBase});
        }
    } else {
        table.push_back({static_cast<float>(INT32_MAX), (data.bullet.damage + adjust) * config.damageBase});
    }
    return table;
}

// EffectiveRangeModifier.initCache: distance of the first damage_adjust entry.
inline float effectiveRange(GunData const& data) {
    auto const& extra = data.bullet.extraDamage;
    if (extra && !extra->damageAdjust.empty()) return extra->damageAdjust.front().distance;
    return static_cast<float>(INT32_MAX);
}

// HeadShotModifier.initCache. Note: without an extra_damage block TACZ yields 0, so headshots
// deal no damage for such guns; this is kept for parity.
inline float headShotMultiplier(GunData const& data, std::optional<FireMode> mode, DamageConfig const& config = {}) {
    float base = data.bullet.extraDamage ? data.bullet.extraDamage->headShotMultiplier : 0;
    if (auto const* a = data.fireModeAdjustFor(mode)) base += a->headShotMultiplier;
    return std::max(base * config.headShotBase, 0.0f);
}

// ArmorIgnoreModifier.initCache, clamped to [0, 1] as in EntityKineticBullet.
inline float armorIgnore(GunData const& data, std::optional<FireMode> mode, DamageConfig const& config = {}) {
    float base = data.bullet.extraDamage ? data.bullet.extraDamage->armorIgnore : 0;
    if (auto const* a = data.fireModeAdjustFor(mode)) base += a->armorIgnore;
    return std::clamp(base * config.armorIgnoreBase, 0.0f, 1.0f);
}

// Everything a bullet needs to compute its damage on hit.
struct BulletDamage {
    std::vector<pack::DistanceDamagePair> table;
    float                                 effectiveRange = 0;
    float                                 headShot       = 0;
    float                                 armorIgnore    = 0;
    float                                 pelletModifier = 1; // 1 / bullet_amount for shotguns

    static BulletDamage from(GunData const& data, std::optional<FireMode> mode, DamageConfig const& config = {}) {
        BulletDamage d;
        d.table          = gun::damageTable(data, mode, config);
        d.effectiveRange = gun::effectiveRange(data);
        d.headShot       = gun::headShotMultiplier(data, mode, config);
        d.armorIgnore    = gun::armorIgnore(data, mode, config);
        if (data.bullet.bulletAmount > 1) d.pelletModifier = 1.0f / static_cast<float>(data.bullet.bulletAmount);
        return d;
    }

    // EntityKineticBullet.getDamage: first entry whose distance exceeds the travelled distance.
    // The first entry uses the effective range instead of its own distance. No match -> 0.
    [[nodiscard]] float atDistance(double distance) const {
        for (size_t i = 0; i < table.size(); ++i) {
            float limit = table[i].distance == table.front().distance ? effectiveRange : table[i].distance;
            if (distance < limit) return std::max(table[i].damage * pelletModifier, 0.0f);
        }
        return 0;
    }

    struct Split {
        float normal;        // reduced by armour
        float armorPiercing; // ignores armour
    };

    // onHitEntity + tacAttackEntity: headshot multiplier, then split by armour ignore.
    [[nodiscard]] Split onHit(double distance, bool headshot) const {
        float damage = atDistance(distance);
        if (headshot) damage *= headShot;
        return {damage * (1 - armorIgnore), damage * armorIgnore};
    }
};

} // namespace tacz::gun
