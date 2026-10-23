#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pack/ResourceId.h"

// `sound_effects` keyframes of a Bedrock animation file (TACZ SoundEffectKeyframesSerializer):
// { "<seconds>": { "effect": "<sound id>" } }. TACZ plays them as the animation reaches each time.
namespace tacz::pack {

struct AnimationSound {
    double     time; // seconds from the animation start
    ResourceId sound;
};

// Animation name -> its sound keyframes in time order. Animations without sounds are omitted;
// keyframes with an unparsable time or id are skipped, as in TACZ.
std::map<std::string, std::vector<AnimationSound>> parseAnimationSounds(nlohmann::json const& animationFile);

} // namespace tacz::pack
