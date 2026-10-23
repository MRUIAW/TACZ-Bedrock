#include "pack/AnimationSounds.h"

#include <algorithm>
#include <charconv>
#include <optional>

namespace tacz::pack {
namespace {

std::optional<double> parseTime(std::string const& text) {
    double value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

} // namespace

std::map<std::string, std::vector<AnimationSound>> parseAnimationSounds(nlohmann::json const& file) {
    std::map<std::string, std::vector<AnimationSound>> out;
    auto animations = file.find("animations");
    if (animations == file.end() || !animations->is_object()) return out;
    for (auto const& [name, animation] : animations->items()) {
        if (!animation.is_object()) continue;
        auto effects = animation.find("sound_effects");
        if (effects == animation.end() || !effects->is_object()) continue;
        std::vector<AnimationSound> sounds;
        for (auto const& [timeText, value] : effects->items()) {
            if (!value.is_object()) continue;
            auto time   = parseTime(timeText);
            auto effect = value.find("effect");
            if (!time || effect == value.end() || !effect->is_string()) continue;
            auto id = ResourceId::parse(effect->get<std::string>());
            if (id) sounds.push_back({*time, *id});
        }
        if (sounds.empty()) continue;
        std::ranges::sort(sounds, {}, &AnimationSound::time);
        out.emplace(name, std::move(sounds));
    }
    return out;
}

} // namespace tacz::pack
