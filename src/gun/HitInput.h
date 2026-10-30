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

inline constexpr uint64_t TeleportAckTimeoutTicks = 100; // five seconds at 20 TPS
enum class TeleportPhase { Mark, Ack, Timeout };
struct TeleportEvent {
    TeleportPhase phase;
    uint64_t tick = 0, waitTicks = 0, ignored = 0;
    glm::dvec3 target{0}; // native teleportTo argument, no coordinate conversion
};

class AuthInputRing {
public:
    void bind(uintptr_t world, uintptr_t actor, int dimension) {
        if (world != worldIdentity || actor != actorIdentity || dimension != dimensionIdentity) clear();
        worldIdentity = world; actorIdentity = actor; dimensionIdentity = dimension;
    }
    bool boundToWorld(uintptr_t world) const { return worldIdentity == world; }
    uint64_t teleportWaitTicks(uint64_t tick) const {
        return awaitingTeleportAck && tick >= teleportStartTick ? tick - teleportStartTick : 0;
    }
    TeleportEvent markTeleport(uint64_t tick, glm::dvec3 target) {
        clear(); // shared aim AND movement-inaccuracy history, intentionally
        awaitingTeleportAck = true; teleportStartTick = tick; teleportTarget = target;
        return {TeleportPhase::Mark, tick, 0, 0, target};
    }
    std::optional<TeleportEvent> expireTeleport(uint64_t tick) {
        if (!awaitingTeleportAck || teleportWaitTicks(tick) < TeleportAckTimeoutTicks) return {};
        auto event = teleportEvent(TeleportPhase::Timeout, tick);
        awaitingTeleportAck = false;
        return event;
    }
    std::optional<TeleportEvent> receive(AuthInput value, bool handledTeleport = false) {
        auto event = expireTeleport(value.receiptTick); // timeout wins at the boundary
        if (awaitingTeleportAck) {
            if (!handledTeleport) { ++ignoredTeleportPackets; return {}; }
            event = teleportEvent(TeleportPhase::Ack, value.receiptTick);
            awaitingTeleportAck = false;
        }
        previous = latest; latest = value;
        return event;
    }
    void clear() {
        previous.reset(); latest.reset(); awaitingTeleportAck = false;
        teleportStartTick = ignoredTeleportPackets = 0; teleportTarget = glm::dvec3{0};
    }
    std::optional<AuthInput> latest, previous;
    bool awaitingTeleportAck = false;
private:
    TeleportEvent teleportEvent(TeleportPhase phase, uint64_t tick) const {
        return {phase, tick, teleportWaitTicks(tick), ignoredTeleportPackets, teleportTarget};
    }
    uint64_t teleportStartTick = 0, ignoredTeleportPackets = 0;
    glm::dvec3 teleportTarget{0};
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
    if (input.awaitingTeleportAck || !input.latest) return {midpointEyeFromFeet(fallbackFeet, fallbackFeet, eyeHeight), directionFromRotation(fallbackRotation)};
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
