#include "pack/GunAnimations.h"

#include "pack/GunPackParser.h"

namespace tacz::pack {

animation::AnimationFile loadGunAnimations(
    GunPackRegistry const& registry,
    GunDisplay const& display,
    animation::LogFn const& warn
) {
    auto load = [&](ResourceId const& id) {
        animation::AnimationFile result;
        try {
            auto text = registry.readFile(assetPath(id, "animations", ".animation.json"));
            if (!text) throw ParseError("file not found");
            result = animation::parseAnimationFile(parseJsonText(*text), warn);
        } catch (std::exception const& e) {
            if (warn) warn("Animation " + id.toString() + ": " + e.what());
        }
        return result;
    };
    if (!display.animation) return {};
    auto result = load(*display.animation);
    std::optional<ResourceId> fallback = display.defaultAnimation;
    if (!fallback && (display.useDefaultAnimation == "rifle" || display.useDefaultAnimation == "pistol")) {
        fallback = ResourceId{"tacz", display.useDefaultAnimation + "_default"};
    }
    if (fallback) result.merge(load(*fallback)); // existing names win, as providePrototypeIfAbsent
    return result;
}

} // namespace tacz::pack
