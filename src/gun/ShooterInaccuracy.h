#pragma once

#include <string_view>
#include "gun/HitInput.h"
#include "gun/ShootRules.h"

namespace tacz::gun {

// Owned preparation-time input and result, retained by the shootOnce cycle.
struct RoundInaccuracy {
    glm::dvec3 movement{0};
    unsigned samples = 0;
    InaccuracyType type = InaccuracyType::Stand;
    float value = 0;
};

inline RoundInaccuracy authInputInaccuracy(AuthInputRing const& input, pack::GunData const& data,
    std::optional<pack::FireMode> mode, bool aiming, bool sneaking) {
    RoundInaccuracy result;
    result.samples = unsigned(input.latest.has_value()) + unsigned(input.previous.has_value());
    // User-chosen receipt-phase analogue of HitboxHelper.getPlayerVelocity.
    // No native-motion fallback, tick normalization or packet-tick filtering.
    if (input.latest && input.previous) result.movement = input.latest->pos - input.previous->pos;
    result.type = inaccuracyType(aiming, sneaking, glm::length(result.movement));
    result.value = inaccuracy(data, mode, result.type);
    return result;
}

inline std::string_view inaccuracyTypeLabel(InaccuracyType type) {
    switch (type) {
    case InaccuracyType::Stand: return "STAND";
    case InaccuracyType::Move: return "MOVE";
    case InaccuracyType::Sneak: return "SNEAK";
    case InaccuracyType::Lie: return "LIE";
    case InaccuracyType::Aim: return "AIM";
    }
    return "UNKNOWN";
}

} // namespace tacz::gun
