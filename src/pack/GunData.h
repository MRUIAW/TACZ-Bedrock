#pragma once

#include <cfloat>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pack/ResourceId.h"

// Gun pack data model. Field names, defaults and JSON keys follow TACZ's POJOs
// (com.tacz.guns.resource.pojo.*) so that the same pack files load identically.
namespace tacz::pack {

enum class Bolt { OpenBolt, ClosedBolt, ManualAction };
enum class FeedType { Magazine, Manual, Fuel, Inventory };
enum class FireMode { Auto, Semi, Burst };
enum class InaccuracyType { Stand, Move, Sneak, Lie, Aim };

// data/guns -> bullet.extra_damage.damage_adjust[]
struct DistanceDamagePair {
    float distance = 0; // "infinite" parses to FLT_MAX
    float damage   = 0;
};

struct ExtraDamage {
    float                           armorIgnore        = 0;
    float                           headShotMultiplier = 1;
    std::vector<DistanceDamagePair> damageAdjust;
};

struct Ignite {
    bool entity = false;
    bool block  = false;
};

struct ExplosionData {
    bool  explode      = false;
    float radius       = 0;
    float damage       = 0;
    bool  knockback    = false;
    bool  destroyBlock = false;
    float delay        = 0; // seconds
};

struct BulletData {
    float                        life                = 10;   // seconds
    int                          bulletAmount        = 1;
    float                        damage              = 5;
    std::optional<ExtraDamage>   extraDamage;
    float                        speed               = 5;    // m/s
    float                        gravity             = 0;
    float                        knockback           = 0;
    float                        friction            = 0.01f;
    int                          pierce              = 1;
    Ignite                       ignite;
    int                          igniteEntityTime    = 2;    // seconds
    int                          tracerCountInterval = -1;   // -1: no tracers
    std::optional<ExplosionData> explosion;
};

struct GunReloadTime {
    float empty    = 2.5f;
    float tactical = 2.0f;
};

struct GunReloadData {
    std::optional<FeedType> type     = FeedType::Magazine;
    bool                    infinite = false;
    GunReloadTime           feed;
    GunReloadTime           cooldown;
};

struct BurstData {
    bool   continuousShoot = false;
    int    count           = 3;
    int    bpm             = 200;
    double minInterval     = 1;
};

struct RecoilKeyFrame {
    float              time = 0;
    std::vector<float> value{0, 0}; // [min, max], validated to length 2
};

struct GunRecoil {
    std::vector<RecoilKeyFrame> pitch;
    std::vector<RecoilKeyFrame> yaw;
};

struct MoveSpeed {
    float base   = 0;
    float aim    = 0;
    float reload = 0;
};

struct FireModeAdjust {
    float damage             = 0;
    int   rpm                = 0;
    float speed              = 0;
    float knockback          = 0;
    float armorIgnore        = 0;
    float headShotMultiplier = 0;
    float aimInaccuracy      = 0;
    float otherInaccuracy    = 0;
};

struct GunHeatData {
    float max               = 100;
    float perShot           = 3;
    float coolingMultiplier = 1;
    long long coolingDelay  = 1000; // ms
    long long overHeatTime  = 3000; // ms
    float minInaccuracy     = 1;
    float maxInaccuracy     = 1;
    float minRpmMod         = 1;
    float maxRpmMod         = 1;
};

struct FireSound {
    float fireMultiplier    = 1;
    float silenceMultiplier = 1;
};

// data/<ns>/data/guns/*.json
struct GunData {
    std::optional<ResourceId>                ammoId;
    int                                      ammoAmount = 30;
    std::optional<std::vector<int>>          extendedMagAmmoAmount;
    bool                                     canCrawl = true;
    bool                                     canSlide = true;
    std::optional<Bolt>                      bolt     = Bolt::OpenBolt;
    int                                      rpm      = 300;
    BulletData                               bullet;
    float                                    drawTime       = 0.4f;
    float                                    putAwayTime    = 0.4f;
    float                                    sprintTime     = 0.2f;
    float                                    aimTime        = 0.2f;
    float                                    boltActionTime = 0;
    float                                    boltFeedTime   = -1;
    FireSound                                fireSound;
    GunReloadData                            reload;
    std::vector<std::optional<FireMode>>     fireModes; // TACZ default is [UNKNOWN], which fails validation
    std::map<FireMode, FireModeAdjust>       fireModeAdjust;
    BurstData                                burst;
    float                                    crawlRecoilMultiplier = 0.5f;
    GunRecoil                                recoil;
    float                                    hurtBobTweakMultiplier = 0.05f;
    std::optional<std::map<InaccuracyType, float>> inaccuracy;
    MoveSpeed                                movementSpeed;
    std::optional<GunHeatData>               heat;
    std::vector<std::string>                 allowAttachmentTypes;
    float                                    weight = 0;
    std::optional<ResourceId>                script;
    // Kept as raw JSON until the systems that use them are ported.
    nlohmann::json                           scriptParam;          // passed to Lua as a table
    nlohmann::json                           melee;
    nlohmann::json                           exclusiveAttachments;
    nlohmann::json                           builtinAttachments;
    nlohmann::json                           charging;

    [[nodiscard]] FireModeAdjust const* fireModeAdjustFor(FireMode mode) const {
        auto it = fireModeAdjust.find(mode);
        return it == fireModeAdjust.end() ? nullptr : &it->second;
    }
};

// data/<ns>/index/guns/*.json
struct GunIndexData {
    std::string               name;
    std::string               tooltip;
    std::optional<ResourceId> display;
    std::optional<ResourceId> data;
    std::string               type;
    std::string               itemType = "modern_kinetic";
    int                       sort     = 0;
};

// data/<ns>/index/ammo/*.json
struct AmmoIndexData {
    std::string               name;
    std::optional<ResourceId> display;
    int                       stackSize = 0;
    std::string               tooltip;
    int                       sort = 0;
};

// gunpack.meta.json
struct PackMeta {
    std::string                        ns;
    std::map<std::string, std::string> dependencies;
};

// TACZ's InaccuracyType.getDefaultInaccuracy(), used to fill missing entries.
inline std::map<InaccuracyType, float> defaultInaccuracy() {
    return {
        {InaccuracyType::Stand, 5.0f },
        {InaccuracyType::Move,  5.75f},
        {InaccuracyType::Sneak, 3.5f },
        {InaccuracyType::Lie,   2.5f },
        {InaccuracyType::Aim,   0.15f},
    };
}

} // namespace tacz::pack
