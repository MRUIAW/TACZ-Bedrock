#include "model/Animation.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <numbers>

#include <glm/gtc/quaternion.hpp>

#include "pack/GunPackParser.h"

namespace tacz::animation {
namespace {
using json = nlohmann::json;

void log(LogFn const& warn, std::string const& message) {
    if (warn) warn(message);
}

glm::vec3 vector(json const& value, LogFn const& warn) {
    if (!value.is_array() || value.size() < 3) throw pack::ParseError("animation: expected three-axis array");
    glm::vec3 result{0};
    for (int i = 0; i < 3; ++i) {
        if (value[i].is_string()) log(warn, "Molang is not supported: " + value[i].get<std::string>());
        else if (value[i].is_number()) result[i] = value[i].get<float>();
        else throw pack::ParseError("animation: invalid vector component");
        if (!std::isfinite(result[i])) throw pack::ParseError("animation: non-finite component");
    }
    return result;
}

std::optional<Keyframe> keyframe(json const& value, ChannelType type, LogFn const& warn) {
    Keyframe key;
    if (value.is_array()) key.pre = key.post = vector(value, warn);
    else if (value.is_object()) {
        bool pre = value.contains("pre") && value["pre"].is_array();
        bool post = value.contains("post") && value["post"].is_array();
        if (!pre && !post) {
            log(warn, "Animation keyframe without pre/post array ignored (Molang is not supported)");
            return std::nullopt;
        }
        key.pre = pre ? vector(value["pre"], warn) : vector(value["post"], warn);
        key.post = post ? vector(value["post"], warn) : key.pre;
        auto mode = value.value("lerp_mode", std::string{});
        // Rotation in Animations.java recognises only the exact lowercase catmullrom name.
        if (type != ChannelType::Rotation) {
            std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        if (mode == "catmullrom") key.mode = LerpMode::CatmullRom;
        else if (type != ChannelType::Rotation && (mode == "spherical_linear" || mode == "spherical_squad")) {
            log(warn, "Spherical interpolation on a non-rotation Bedrock channel is not supported; using linear");
        }
    } else {
        log(warn, "Non-array animation keyframe ignored (Molang is not supported)");
        return std::nullopt;
    }
    float factor = type == ChannelType::Translation ? 1.f / 16.f
                 : type == ChannelType::Rotation ? std::numbers::pi_v<float> / 180.f : 1.f;
    key.pre *= factor;
    key.post *= factor;
    return key;
}

Channel parseChannel(std::string const& bone, ChannelType type, json const& value, LogFn const& warn) {
    Channel channel{bone, type, {}};
    if (value.is_string()) {
        log(warn, "Molang is not supported: " + value.get<std::string>());
    } else if (value.is_number() || value.is_array()) {
        // A scalar at channel level is replicated on all three axes by TACZ's serializer.
        auto v = value.is_number() ? json::array({value, value, value}) : value;
        if (auto k = keyframe(v, type, warn)) channel.keys.push_back(*k);
    } else if (value.is_object()) {
        std::map<double, Keyframe> sorted;
        for (auto const& [time, v] : value.items()) {
            double seconds;
            try {
                size_t end = 0;
                seconds = std::stod(time, &end);
                if (end != time.size() || !std::isfinite(seconds) || !std::isfinite(static_cast<float>(seconds))) {
                    throw std::invalid_argument("time");
                }
            } catch (...) {
                log(warn, "Molang is not supported: " + time);
                break;
            }
            if (auto k = keyframe(v, type, warn)) {
                k->time = static_cast<float>(seconds);
                sorted[seconds] = *k;
            }
        }
        for (auto const& [time, k] : sorted) channel.keys.push_back(k);
    }
    return channel;
}
} // namespace

glm::vec3 Channel::evaluate(float seconds) const {
    if (keys.empty()) return type == ChannelType::Scale ? glm::vec3{1} : glm::vec3{0};
    auto upper = std::upper_bound(keys.begin(), keys.end(), seconds, [](float t, Keyframe const& k) { return t < k.time; });
    size_t from = upper == keys.begin() ? 0 : static_cast<size_t>(upper - keys.begin() - 1);
    size_t to = std::min(from + 1, keys.size() - 1);
    float alpha = seconds <= keys.front().time ? 0.f : seconds >= keys.back().time ? 1.f
                : (seconds - keys[from].time) / (keys[to].time - keys[from].time);
    auto const& a = keys[from];
    auto const& b = keys[to];
    if (a.mode == LerpMode::CatmullRom || b.mode == LerpMode::CatmullRom) {
        if (keys.size() == 1) return alpha > 0 ? a.post : a.pre;
        // CustomInterpolator deliberately takes from.pre (not from.post) here, and the
        // preceding key's post. Endpoint neighbours are duplicated, even when from == to.
        auto p = keys[from == 0 ? 0 : from - 1].post;
        auto n = keys[to == keys.size() - 1 ? to : to + 1].pre;
        float t2 = alpha * alpha, t3 = alpha * t2;
        return (2.f * t3 - 3.f * t2 + 1.f) * a.pre + (-2.f * t3 + 3.f * t2) * b.pre
             + (t3 - 2.f * t2 + alpha) * ((b.pre - p) * 0.5f)
             + (t3 - t2) * ((n - a.pre) * 0.5f);
    }
    if (from == to) return alpha > 0 ? b.post : b.pre;
    return a.post * (1.f - alpha) + b.pre * alpha;
}

AnimationFile parseAnimationFile(json const& json, LogFn const& warn) {
    if (!json.is_object() || !json.contains("animations") || !json["animations"].is_object()) {
        throw pack::ParseError("animation: missing animations object");
    }
    AnimationFile file;
    auto sounds = pack::parseAnimationSounds(json);
    for (auto const& [name, value] : json["animations"].items()) {
        Animation animation;
        if (auto it = sounds.find(name); it != sounds.end()) animation.sounds = it->second;
        if (auto loop = value.find("loop"); loop != value.end()) {
            if (*loop == true) animation.playType = PlayType::Loop;
            else if (*loop == "hold_on_last_frame") animation.playType = PlayType::Hold;
        }
        if (auto bones = value.find("bones"); bones != value.end() && bones->is_object()) {
            for (auto const& [bone, channels] : bones->items()) {
                for (auto const& [key, type] : std::initializer_list<std::pair<char const*, ChannelType>>{
                         {"position", ChannelType::Translation}, {"rotation", ChannelType::Rotation}, {"scale", ChannelType::Scale}}) {
                    auto it = channels.find(key);
                    if (it == channels.end()) continue;
                    auto channel = parseChannel(bone, type, *it, warn);
                    if (channel.keys.empty()) continue; // TACZ cannot evaluate empty Molang channels safely
                    animation.duration = std::max(animation.duration, channel.keys.back().time);
                    animation.channels.push_back(std::move(channel));
                }
            }
        }
        // Animations.java ignores animation_length; ObjectAnimation uses the last channel /
        // sound key instead. Do not extend/trim a timeline to the Bedrock metadata length.
        if (auto sounds = value.find("sound_effects"); sounds != value.end() && sounds->is_object()) {
            for (auto const& [time, sound] : sounds->items()) {
                try {
                    float t = std::stof(time);
                    if (std::isfinite(t)) animation.duration = std::max(animation.duration, t);
                } catch (...) {}
            }
        }
        file.emplace(name, std::move(animation));
    }
    return file;
}

std::optional<float> playbackTime(Animation const& animation, double seconds, PlayType type) {
    if (seconds > animation.duration) {
        if (type == PlayType::Stop) return std::nullopt;
        if (type == PlayType::Hold) return static_cast<float>(static_cast<double>(animation.duration) + 1e-9);
        seconds = animation.duration == 0 ? 0 : std::fmod(seconds, animation.duration);
    }
    return static_cast<float>(seconds);
}

void evaluate(Animation const& animation, float seconds, Pose& pose, bool blend) {
    Pose values;
    for (auto const& channel : animation.channels) {
        auto value = channel.evaluate(seconds);
        auto& bone = values[channel.bone];
        auto& target = channel.type == ChannelType::Translation ? bone.translation
                     : channel.type == ChannelType::Rotation ? bone.rotation : bone.scale;
        target = value;
    }
    compose(values, pose, blend);
}

void compose(Pose const& source, Pose& pose, bool blend) {
    for (auto const& [name, input] : source) {
        auto& bone = pose[name];
        for (auto type : {ChannelType::Translation, ChannelType::Rotation, ChannelType::Scale}) {
            auto const& src = type == ChannelType::Translation ? input.translation : type == ChannelType::Rotation ? input.rotation : input.scale;
            if (!src) continue;
            auto value = *src;
            auto& target = type == ChannelType::Translation ? bone.translation : type == ChannelType::Rotation ? bone.rotation : bone.scale;
            if (blend && target) {
                if (type == ChannelType::Scale) value *= *target;
                else if (type == ChannelType::Rotation) {
                    // ModelRotateListener reads the existing additionalQuaternion back as Euler
                    // angles first. Canonicalise, rather than adding unbounded source angles.
                    auto q = glm::quat(*target);
                    glm::vec3 angles{
                        std::atan2(2.f * (q.w*q.x + q.y*q.z), 1.f - 2.f * (q.x*q.x + q.y*q.y)),
                        std::asin(std::clamp(2.f * (q.w*q.y - q.x*q.z), -1.f, 1.f)),
                        std::atan2(2.f * (q.w*q.z + q.y*q.x), 1.f - 2.f * (q.y*q.y + q.z*q.z))
                    };
                    value += angles;
                } else value += *target;
            }
            target = value;
        }
    }
}

} // namespace tacz::animation
