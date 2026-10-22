#include "pack/GunPackParser.h"

#include <cfloat>
#include <string>
#include <utility>

namespace tacz::pack {
namespace {

using json = nlohmann::json;

[[noreturn]] void fail(std::string_view key, std::string_view expected) {
    throw ParseError("field '" + std::string(key) + "' is not " + std::string(expected));
}

// Field readers: a missing or null field keeps the default, a wrong type fails the file
// (Gson behaviour).
void read(json const& obj, std::string_view key, bool& out) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return;
    if (!it->is_boolean()) fail(key, "a boolean");
    out = it->get<bool>();
}

template <class T>
    requires std::is_arithmetic_v<T>
void read(json const& obj, std::string_view key, T& out) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return;
    if (!it->is_number()) fail(key, "a number");
    out = it->get<T>();
}

void read(json const& obj, std::string_view key, std::string& out) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return;
    if (!it->is_string()) fail(key, "a string");
    out = it->get<std::string>();
}

ResourceId toResourceId(json const& value, std::string_view key) {
    if (!value.is_string()) fail(key, "a resource id string");
    auto id = ResourceId::parse(value.get<std::string>());
    if (!id) throw ParseError("field '" + std::string(key) + "' is not a valid resource id: " + value.get<std::string>());
    return *id;
}

void read(json const& obj, std::string_view key, std::optional<ResourceId>& out) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return;
    out = toResourceId(*it, key);
}

json const* object(json const& obj, std::string_view key) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return nullptr;
    if (!it->is_object()) fail(key, "an object");
    return &*it;
}

json const* array(json const& obj, std::string_view key) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return nullptr;
    if (!it->is_array()) fail(key, "an array");
    return &*it;
}

json raw(json const& obj, std::string_view key) {
    auto it = obj.find(key);
    return it == obj.end() ? json{} : *it;
}

// Gson maps an unknown enum name to null rather than failing.
std::optional<Bolt> parseBolt(std::string_view s) {
    if (s == "open_bolt") return Bolt::OpenBolt;
    if (s == "closed_bolt") return Bolt::ClosedBolt;
    if (s == "manual_action") return Bolt::ManualAction;
    return std::nullopt;
}

std::optional<FeedType> parseFeedType(std::string_view s) {
    if (s == "magazine") return FeedType::Magazine;
    if (s == "manual") return FeedType::Manual;
    if (s == "fuel") return FeedType::Fuel;
    if (s == "inventory") return FeedType::Inventory;
    return std::nullopt;
}

std::optional<FireMode> parseFireMode(std::string_view s) {
    if (s == "auto") return FireMode::Auto;
    if (s == "semi") return FireMode::Semi;
    if (s == "burst") return FireMode::Burst;
    return std::nullopt; // includes "unknown"
}

std::optional<InaccuracyType> parseInaccuracyType(std::string_view s) {
    if (s == "stand") return InaccuracyType::Stand;
    if (s == "move") return InaccuracyType::Move;
    if (s == "sneak") return InaccuracyType::Sneak;
    if (s == "lie") return InaccuracyType::Lie;
    if (s == "aim") return InaccuracyType::Aim;
    return std::nullopt;
}

template <class E>
void readEnum(json const& obj, std::string_view key, std::optional<E>& out, std::optional<E> (*parse)(std::string_view)) {
    auto it = obj.find(key);
    if (it == obj.end()) return;
    if (it->is_null()) {
        out = std::nullopt;
        return;
    }
    if (!it->is_string()) fail(key, "a string");
    out = parse(it->get<std::string>());
}

// DistanceDamagePairSerializer: distance is a number or the string "infinite".
DistanceDamagePair parseDistanceDamage(json const& j) {
    if (!j.is_object()) throw ParseError("damage_adjust entry is not an object");
    if (!j.contains("distance")) throw ParseError("damage_adjust entry has no distance field");
    if (!j.contains("damage")) throw ParseError("damage_adjust entry has no damage field");
    auto const& distance = j["distance"];
    auto const& damage   = j["damage"];
    if (!damage.is_number()) throw ParseError("damage_adjust damage is not a number");
    DistanceDamagePair pair;
    if (distance.is_number()) {
        pair.distance = distance.get<float>();
    } else if (distance.is_string() && distance.get<std::string>() == "infinite") {
        pair.distance = FLT_MAX;
    } else {
        throw ParseError("damage_adjust distance is neither a number nor 'infinite'");
    }
    pair.damage = damage.get<float>();
    return pair;
}

ExtraDamage parseExtraDamage(json const& j) {
    ExtraDamage extra;
    read(j, "armor_ignore", extra.armorIgnore);
    read(j, "head_shot_multiplier", extra.headShotMultiplier);
    if (auto* list = array(j, "damage_adjust")) {
        for (auto const& entry : *list) extra.damageAdjust.push_back(parseDistanceDamage(entry));
    }
    return extra;
}

// IgniteSerializer: either a boolean (both flags) or {entity, block}.
Ignite parseIgnite(json const& j) {
    if (j.is_boolean()) return {j.get<bool>(), j.get<bool>()};
    if (j.is_object()) {
        Ignite ignite;
        read(j, "entity", ignite.entity);
        read(j, "block", ignite.block);
        return ignite;
    }
    throw ParseError("field 'ignite' is neither a boolean nor an object");
}

ExplosionData parseExplosion(json const& j) {
    // TACZ's ExplosionData has no no-arg constructor, so Gson leaves absent fields at zero.
    ExplosionData data;
    read(j, "explode", data.explode);
    read(j, "radius", data.radius);
    read(j, "damage", data.damage);
    read(j, "knockback", data.knockback);
    read(j, "destroy_block", data.destroyBlock);
    read(j, "delay", data.delay);
    return data;
}

BulletData parseBullet(json const& j) {
    BulletData bullet;
    read(j, "life", bullet.life);
    read(j, "bullet_amount", bullet.bulletAmount);
    read(j, "damage", bullet.damage);
    if (auto* extra = object(j, "extra_damage")) bullet.extraDamage = parseExtraDamage(*extra);
    read(j, "speed", bullet.speed);
    read(j, "gravity", bullet.gravity);
    read(j, "knockback", bullet.knockback);
    read(j, "friction", bullet.friction);
    read(j, "pierce", bullet.pierce);
    if (auto it = j.find("ignite"); it != j.end() && !it->is_null()) bullet.ignite = parseIgnite(*it);
    read(j, "ignite_entity_time", bullet.igniteEntityTime);
    read(j, "tracer_count_interval", bullet.tracerCountInterval);
    if (auto* explosion = object(j, "explosion")) bullet.explosion = parseExplosion(*explosion);
    return bullet;
}

GunReloadTime parseReloadTime(json const& j) {
    GunReloadTime time;
    read(j, "empty", time.empty);
    read(j, "tactical", time.tactical);
    return time;
}

GunReloadData parseReload(json const& j) {
    GunReloadData reload;
    readEnum(j, "type", reload.type, parseFeedType);
    read(j, "infinite", reload.infinite);
    if (auto* feed = object(j, "feed")) reload.feed = parseReloadTime(*feed);
    if (auto* cooldown = object(j, "cooldown")) reload.cooldown = parseReloadTime(*cooldown);
    return reload;
}

std::vector<RecoilKeyFrame> parseRecoilFrames(json const& j, std::string_view key) {
    std::vector<RecoilKeyFrame> frames;
    auto* list = array(j, key);
    if (!list) return frames;
    for (auto const& entry : *list) {
        if (!entry.is_object()) throw ParseError("recoil keyframe is not an object");
        RecoilKeyFrame frame;
        read(entry, "time", frame.time);
        if (auto* value = array(entry, "value")) {
            frame.value.clear();
            for (auto const& v : *value) {
                if (!v.is_number()) throw ParseError("recoil value is not a number");
                frame.value.push_back(v.get<float>());
            }
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

FireModeAdjust parseFireModeAdjust(json const& j) {
    FireModeAdjust adjust;
    read(j, "damage", adjust.damage);
    read(j, "rpm", adjust.rpm);
    read(j, "speed", adjust.speed);
    read(j, "knockback", adjust.knockback);
    read(j, "armor_ignore", adjust.armorIgnore);
    read(j, "head_shot_multiplier", adjust.headShotMultiplier);
    read(j, "aim_inaccuracy", adjust.aimInaccuracy);
    read(j, "other_inaccuracy", adjust.otherInaccuracy);
    return adjust;
}

GunHeatData parseHeat(json const& j) {
    GunHeatData heat;
    read(j, "max", heat.max);
    read(j, "per_shot", heat.perShot);
    read(j, "cooling_multiplier", heat.coolingMultiplier);
    read(j, "cooling_delay", heat.coolingDelay);
    read(j, "over_heat_time", heat.overHeatTime);
    read(j, "min_inaccuracy", heat.minInaccuracy);
    read(j, "max_inaccuracy", heat.maxInaccuracy);
    read(j, "min_rpm_mod", heat.minRpmMod);
    read(j, "max_rpm_mod", heat.maxRpmMod);
    return heat;
}

void requireObject(json const& j, std::string_view what) {
    if (!j.is_object()) throw ParseError(std::string(what) + " is not a JSON object");
}

} // namespace

nlohmann::json parseJsonText(std::string_view text) {
    // Skip a UTF-8 BOM; several pack files are saved with one.
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    try {
        return json::parse(text, nullptr, true, true);
    } catch (json::parse_error const& e) {
        throw ParseError(e.what());
    }
}

PackMeta parsePackMeta(nlohmann::json const& j) {
    requireObject(j, "gunpack.meta.json");
    PackMeta meta;
    read(j, "namespace", meta.ns);
    if (auto* deps = object(j, "dependencies")) {
        for (auto const& [mod, version] : deps->items()) {
            if (!version.is_string()) fail("dependencies", "a map of strings");
            meta.dependencies[mod] = version.get<std::string>();
        }
    }
    return meta;
}

GunData parseGunData(nlohmann::json const& j) {
    requireObject(j, "gun data");
    GunData data;
    read(j, "ammo", data.ammoId);
    read(j, "ammo_amount", data.ammoAmount);
    if (auto* extended = array(j, "extended_mag_ammo_amount")) {
        std::vector<int> amounts;
        for (auto const& v : *extended) {
            if (!v.is_number()) fail("extended_mag_ammo_amount", "an array of numbers");
            amounts.push_back(v.get<int>());
        }
        data.extendedMagAmmoAmount = std::move(amounts);
    }
    read(j, "can_crawl", data.canCrawl);
    read(j, "can_slide", data.canSlide);
    readEnum(j, "bolt", data.bolt, parseBolt);
    read(j, "rpm", data.rpm);
    if (auto* bullet = object(j, "bullet")) data.bullet = parseBullet(*bullet);
    read(j, "draw_time", data.drawTime);
    read(j, "put_away_time", data.putAwayTime);
    read(j, "sprint_time", data.sprintTime);
    read(j, "aim_time", data.aimTime);
    read(j, "bolt_action_time", data.boltActionTime);
    read(j, "bolt_feed_time", data.boltFeedTime);
    if (auto* sound = object(j, "fire_sound")) {
        read(*sound, "fire_multiplier", data.fireSound.fireMultiplier);
        read(*sound, "silence_multiplier", data.fireSound.silenceMultiplier);
    }
    if (auto* reload = object(j, "reload")) data.reload = parseReload(*reload);
    if (auto* modes = array(j, "fire_mode")) {
        for (auto const& v : *modes) {
            if (!v.is_string()) fail("fire_mode", "an array of strings");
            data.fireModes.push_back(parseFireMode(v.get<std::string>()));
        }
    } else {
        data.fireModes.push_back(std::nullopt); // TACZ default: [UNKNOWN]
    }
    if (auto* adjust = object(j, "fire_mode_adjust")) {
        for (auto const& [name, value] : adjust->items()) {
            auto mode = parseFireMode(name);
            if (!mode || value.is_null()) continue;
            requireObject(value, "fire_mode_adjust entry");
            data.fireModeAdjust[*mode] = parseFireModeAdjust(value);
        }
    }
    if (auto* burst = object(j, "burst_data")) {
        read(*burst, "continuous_shoot", data.burst.continuousShoot);
        read(*burst, "count", data.burst.count);
        read(*burst, "bpm", data.burst.bpm);
        read(*burst, "min_interval", data.burst.minInterval);
    }
    read(j, "crawl_recoil_multiplier", data.crawlRecoilMultiplier);
    if (auto* recoil = object(j, "recoil")) {
        data.recoil.pitch = parseRecoilFrames(*recoil, "pitch");
        data.recoil.yaw   = parseRecoilFrames(*recoil, "yaw");
    }
    read(j, "hurt_bob_tweak_multiplier", data.hurtBobTweakMultiplier);
    if (auto* inaccuracy = object(j, "inaccuracy")) {
        std::map<InaccuracyType, float> values;
        for (auto const& [name, value] : inaccuracy->items()) {
            if (!value.is_number()) fail("inaccuracy", "a map of numbers");
            if (auto type = parseInaccuracyType(name)) values[*type] = value.get<float>();
        }
        data.inaccuracy = std::move(values);
    }
    if (auto* speed = object(j, "movement_speed")) {
        read(*speed, "base", data.movementSpeed.base);
        read(*speed, "aim", data.movementSpeed.aim);
        read(*speed, "reload", data.movementSpeed.reload);
    }
    if (auto* heat = object(j, "heat")) data.heat = parseHeat(*heat);
    if (auto* types = array(j, "allow_attachment_types")) {
        for (auto const& v : *types) {
            if (!v.is_string()) fail("allow_attachment_types", "an array of strings");
            data.allowAttachmentTypes.push_back(v.get<std::string>());
        }
    }
    read(j, "weight", data.weight);
    read(j, "script", data.script);
    data.scriptParam          = raw(j, "script_param");
    data.melee                = raw(j, "melee");
    data.exclusiveAttachments = raw(j, "exclusive_attachments");
    data.builtinAttachments   = raw(j, "builtin_attachments");
    data.charging             = raw(j, "charging");
    return data;
}

GunIndexData parseGunIndex(nlohmann::json const& j) {
    requireObject(j, "gun index");
    GunIndexData index;
    read(j, "name", index.name);
    read(j, "tooltip", index.tooltip);
    read(j, "display", index.display);
    read(j, "data", index.data);
    read(j, "type", index.type);
    read(j, "item_type", index.itemType);
    read(j, "sort", index.sort);
    return index;
}

AmmoIndexData parseAmmoIndex(nlohmann::json const& j) {
    requireObject(j, "ammo index");
    AmmoIndexData index;
    read(j, "name", index.name);
    read(j, "display", index.display);
    read(j, "stack_size", index.stackSize);
    read(j, "tooltip", index.tooltip);
    read(j, "sort", index.sort);
    return index;
}

} // namespace tacz::pack
