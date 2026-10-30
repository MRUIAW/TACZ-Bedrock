#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <iterator>
#include <numbers>
#include <optional>
#include <utility>
#include <glm/glm.hpp>

namespace tacz::gun {

// Raw received Bedrock movement data, never shoot-packet aim data.
struct AuthInput {
    glm::dvec3 pos{0}; // assumed Bedrock base anchor feet + 1.62; pending live evidence
    glm::dvec2 rot{0}; // pitch, yaw (degrees), intentionally not clamped
    float headYaw = 0;
    uint64_t clientTick = 0;
    uint64_t receiptTick = 0;
};

class AuthInputRing {
public:
    void bind(uintptr_t world, uintptr_t actor, int dimension) {
        if (world != worldIdentity || actor != actorIdentity || dimension != dimensionIdentity) clear();
        worldIdentity = world; actorIdentity = actor; dimensionIdentity = dimension;
    }
    void receive(AuthInput value) { previous = latest; latest = value; }
    void clear() { previous.reset(); latest.reset(); }
    std::optional<AuthInput> latest, previous;
private:
    uintptr_t worldIdentity = 0, actorIdentity = 0;
    int dimensionIdentity = 0;
};

inline glm::dvec3 directionFromRotation(glm::dvec2 rotation) {
    double pitch = rotation.x * std::numbers::pi / 180.0;
    double yaw = rotation.y * std::numbers::pi / 180.0;
    return {-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
}

inline double javaEyeHeight(bool sneaking, bool swimming, bool gliding, bool crawling) {
    if (swimming || gliding || crawling) return 0.4;
    return sneaking ? 1.27 : 1.62;
}

// The ONLY packet-anchor conversion assumption. If live authPos/feetPos refutes
// feet + 1.62, change this line, not the Java pose-dependent eye-height function.
inline glm::dvec3 authInputFeet(glm::dvec3 position) {
    return position - glm::dvec3{0, 1.62, 0};
}

inline glm::dvec3 midpointEyeFromFeet(glm::dvec3 previous, glm::dvec3 current, double eyeHeight) {
    return (previous + current) * 0.5 + glm::dvec3{0, eyeHeight, 0};
}

struct RoundAim {
    glm::dvec3 origin{0}, direction{0, 0, 1};
};

inline RoundAim roundAim(AuthInputRing const& input, glm::dvec3 fallbackFeet,
    glm::dvec2 fallbackRotation, double eyeHeight) {
    if (!input.latest) return {midpointEyeFromFeet(fallbackFeet, fallbackFeet, eyeHeight), directionFromRotation(fallbackRotation)};
    auto previous = input.previous.value_or(*input.latest).pos;
    return {midpointEyeFromFeet(authInputFeet(previous), authInputFeet(input.latest->pos), eyeHeight),
        directionFromRotation(input.latest->rot)};
}

// One prior PRE-subTick sample, replaced only AFTER all bullet tests. New,
// missing, over-cap or dimension-changed actors use zero (Java spawn xOld=x).
class DisplacementHistory {
public:
    struct Position { glm::dvec3 pos{0}; int dimension = 0; };
    using Samples = std::map<int64_t, Position>;
    static constexpr size_t Limit = 4096;
    glm::dvec3 velocity(int64_t id, glm::dvec3 current, int dimension) const {
        auto found = previous.find(id);
        if (found == previous.end() || found->second.dimension != dimension) return glm::dvec3{0};
        return current - found->second.pos;
    }
    void sample(Samples positions) {
        while (positions.size() > Limit) positions.erase(std::prev(positions.end()));
        previous = std::move(positions);
    }
    void clear() { previous.clear(); }
private:
    Samples previous;
};

} // namespace tacz::gun
