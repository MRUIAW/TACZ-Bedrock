#include "pack/GunPackParser.h"

#include <cfloat>
#include <string>
#include <utility>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <charconv>
#include <regex>
#include "pack/LenientJson.h"

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
    if (it->is_boolean()) out=it->get<bool>();
    else if (it->is_string()) {
        auto s=it->get<std::string>();
        std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        out=s=="true"; // Gson TypeAdapters.BOOLEAN / Boolean.parseBoolean
    } else fail(key, "a boolean or boolean string");
}

template <class T> T numeric(json const& value,std::string_view key) {
    if (value.is_number()) return value.get<T>();
    if (!value.is_string()) fail(key,"a number or numeric string");
    try {
        auto s=value.get<std::string>();
        if constexpr (std::is_integral_v<T>) {
            // TACZ first builds a JsonElement, then deserializes its POJO. On a
            // string JsonTreeReader.nextInt uses Integer.parseInt, NOT the more
            // permissive streaming nextInt fallback through Double.parseDouble.
            size_t begin=!s.empty() && s[0]=='+' ? 1 : 0;
            if (begin && begin<s.size() && s[begin]=='-') fail(key,"an integer string");
            if (begin<s.size()) {
                T exact{};
                auto result=std::from_chars(s.data()+begin,s.data()+s.size(),exact);
                if (result.ec==std::errc{} && result.ptr==s.data()+s.size()) return exact;
            }
            fail(key,"an integer string");
        }
        // Float/Double.parseDouble accepts Java decimal/hex floating literals,
        // optional suffix and ASCII trim; reject C-only lowercase nan/inf forms.
        static std::regex const floating(R"(^[\x00-\x20]*[+-]?(NaN|Infinity|((([0-9]+(\.[0-9]*)?)|(\.[0-9]+))([eE][+-]?[0-9]+)?|0[xX]([0-9a-fA-F]+(\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)[pP][+-]?[0-9]+)[fFdD]?)[\x00-\x20]*$)");
        if (!std::regex_match(s,floating)) fail(key,"a numeric string");
        auto last=s.find_last_not_of(" \t\r\n\f");
        if (last!=std::string::npos && std::string_view("fFdD").find(s[last])!=std::string_view::npos) s.erase(last,1);
        size_t end=0;
        long double v=std::stold(s,&end);
        while (end<s.size() && std::isspace(static_cast<unsigned char>(s[end]))) ++end;
        if (end!=s.size()) fail(key,"a numeric string");
        return static_cast<T>(v);
    } catch (std::exception const&) { fail(key,"a numeric string"); }
}

template <class T>
    requires std::is_arithmetic_v<T>
void read(json const& obj, std::string_view key, T& out) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return;
    out = numeric<T>(*it,key);
}

void read(json const& obj, std::string_view key, std::string& out) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return;
    if (it->is_string()) out=it->get<std::string>();
    else if (it->is_number() || it->is_boolean()) out=it->dump();
    else fail(key,"a primitive string");
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
    DistanceDamagePair pair;
    if (distance.is_number()) {
        pair.distance = distance.get<float>();
    } else if (distance.is_string() && distance.get<std::string>()=="infinite") {
        pair.distance = FLT_MAX;
    } else if (distance.is_boolean()) {
        pair.distance = 0; // literal behavior of TACZ DistanceDamagePairSerializer
    } else {
        throw ParseError("damage_adjust distance is neither a number nor 'infinite'");
    }
    pair.damage = numeric<float>(damage,"damage");
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
        // This custom serializer uses GsonHelper, not TypeAdapters.BOOLEAN.
        auto strict=[&](char const* key,bool& out) {
            if (auto it=j.find(key); it!=j.end()) {
                if (!it->is_boolean()) fail(key,"a boolean");
                out=it->get<bool>();
            }
        };
        strict("entity",ignite.entity);
        strict("block",ignite.block);
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
                frame.value.push_back(numeric<float>(v,"recoil.value"));
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
    // Strict/comments fast path, then Gson JsonReader LENIENT syntax. Do not remove
    // trailing array commas: Gson inserts a null entry, not an absent element.
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    try {
        return json::parse(text, nullptr, true, true);
    } catch (json::parse_error const&) {
        return readLenientJson(text);
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
            amounts.push_back(numeric<int>(v,"extended_mag_ammo_amount"));
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
            if (v.is_null()) data.fireModes.push_back(std::nullopt);
            else if (v.is_string()) data.fireModes.push_back(parseFireMode(v.get<std::string>()));
            else fail("fire_mode", "an array of strings/null");
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
            if (auto type = parseInaccuracyType(name)) values[*type] = numeric<float>(value,"inaccuracy");
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
            // Gson's enum adapter calls nextString: NUMBER text cannot name an
            // AttachmentType, so it becomes null; BOOLEAN still fails the file.
            if (v.is_null() || v.is_number()) data.allowAttachmentTypes.push_back(std::nullopt);
            else if (v.is_string()) data.allowAttachmentTypes.push_back(v.get<std::string>());
            else fail("allow_attachment_types", "an array of strings/numbers/null");
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

GunDisplay parseGunDisplay(nlohmann::json const& j) {
    requireObject(j, "gun display");
    GunDisplay display;
    read(j, "model", display.model);
    read(j, "texture", display.texture);
    read(j, "animation", display.animation);
    read(j, "default_animation", display.defaultAnimation);
    read(j, "state_machine", display.stateMachine);
    if (auto* params = object(j, "state_machine_param")) display.stateMachineParam = *params;
    read(j, "use_default_animation", display.useDefaultAnimation);
    read(j, "iron_zoom", display.ironZoom);
    read(j, "zoom_model_fov", display.zoomModelFov);
    read(j, "show_crosshair", display.showCrosshair);
    read(j, "enable_transparency", display.enableTransparency);
    read(j,"hud",display.hud);
    read(j,"hud_empty",display.hudEmpty);
    std::string countStyle;
    read(j,"ammo_count_style",countStyle);
    display.ammoPercent=countStyle=="percent";
    if (auto* sounds = object(j, "sounds")) {
        for (auto const& [key, value] : sounds->items()) {
            // An empty id is a valid (path-less) ResourceLocation in Java and simply plays
            // nothing; several default-pack displays use it to silence a sound.
            if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) continue;
            display.sounds.emplace(key, toResourceId(value, "sounds." + key));
        }
    }
    // GunDisplayInstance: sounds every gun gets unless the display names its own.
    for (auto const& [key, path] : std::initializer_list<std::pair<char const*, char const*>>{
             {"dry_fire",      "dry_fire"                       },
             {"fire_select",   "fire_select"                    },
             {"head_hit",      "head_hit"                       },
             {"flesh_hit",     "flesh_hit"                      },
             {"kill",          "kill"                           },
             {"melee_bayonet", "melee_bayonet/melee_bayonet_01"},
             {"melee_stock",   "melee_stock/melee_stock_01"    },
             {"melee_push",    "melee_stock/melee_stock_02"    },
    }) {
        display.sounds.try_emplace(key, ResourceId{"tacz", path});
    }
    display.raw = j;
    return display;
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
