#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "pack/AnimationSounds.h"

namespace tacz::animation {

using LogFn = std::function<void(std::string const&)>;
enum class ChannelType { Translation, Rotation, Scale };
enum class LerpMode { Linear, CatmullRom };
enum class PlayType { Loop, Hold, Stop };

struct Keyframe {
    float     time = 0;
    glm::vec3 pre{0};
    glm::vec3 post{0}; // equals pre unless both were supplied
    LerpMode  mode = LerpMode::Linear;
};

struct Channel {
    std::string           bone;
    ChannelType           type;
    std::vector<Keyframe> keys;

    [[nodiscard]] glm::vec3 evaluate(float seconds) const;
};

struct BonePose {
    std::optional<glm::vec3> translation; // blocks, animation coordinates (y up)
    std::optional<glm::vec3> rotation;    // radians, additional Rz * Ry * Rx
    std::optional<glm::vec3> scale;

    bool operator==(BonePose const&) const = default;
};
using Pose = std::map<std::string, BonePose>;

struct Animation {
    std::vector<Channel> channels;
    std::vector<pack::AnimationSound> sounds;
    float               duration = 0; // ObjectAnimation.maxEndTimeS, including sound keys
    PlayType            playType = PlayType::Stop;
};
using AnimationFile = std::map<std::string, Animation>;

// AnimationKeyframesSerializer + Animations.writeBedrock*, no game dependencies.
AnimationFile parseAnimationFile(nlohmann::json const& json, LogFn const& warn = {});

// Runner boundary is strictly > end, not >=. A stopped track contributes nothing.
std::optional<float> playbackTime(Animation const& animation, double seconds, PlayType type);
void evaluate(Animation const& animation, float seconds, Pose& pose, bool blend = false);
void compose(Pose const& source, Pose& pose, bool blend);

} // namespace tacz::animation
