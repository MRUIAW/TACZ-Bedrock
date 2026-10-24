#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "pack/ResourceId.h"

namespace tacz::gun {

inline int64_t animationMillis(float seconds) {
    // Java multiplies the float by 1000 before casting; promoting 0.57F to double first
    // would truncate AK47's nominal 570 ms to 569 ms.
    return std::isfinite(seconds) ? static_cast<int64_t>(std::clamp(seconds, 0.f, 3600.f) * 1000.f) : 0;
}

// LivingEntityDrawGun: a second switch during put-away preserves its future draw timestamp;
// switching early during draw carries only the elapsed draw time, capped by old put-away.
struct DrawCooldown {
    std::optional<int64_t> drawTimestamp;
    int64_t previousPutAway = 0;

    void select(int64_t now, int64_t putAway) {
        if (!drawTimestamp) drawTimestamp = now;
        int64_t elapsed = now - *drawTimestamp;
        if (elapsed >= 0) drawTimestamp = now + std::min(elapsed, previousPutAway);
        previousPutAway = putAway;
    }
    int64_t remaining(int64_t now, int64_t drawTime, int64_t window = 5) const {
        return drawTimestamp ? std::max<int64_t>(drawTime - (now - *drawTimestamp) - window, 0) : 0;
    }
};

struct HandSelection {
    std::optional<pack::ResourceId> gun;
    int slot = -1;
    bool operator==(HandSelection const&) const = default;
};

// SimpleBedrockModel FirstPersonRenderHandler: one visible instance and one latest target,
// never a queue. The adapter owns the old ItemStack/VM until Reveal, not the selected slot.
class HandSwitch {
public:
    enum class Event { None, PutAway, Reveal };
    static constexpr int64_t ExitGuardMs = 50;

    Event select(HandSelection const& next, int64_t now, int64_t oldPutAway, int64_t nextDraw) {
        bool changed = next != selected;
        selected = next;
        pendingDraw = nextDraw;
        if (puttingAway) return Event::None; // replace pending target without restarting the old gun
        if (!changed) return Event::None;
        if (visible.gun) {
            puttingAway = true;
            putAwayEnd = now + oldPutAway;
            // +50 ms is TACZ's exit/re-init guard. We explicitly retain this guard for the
            // requested Bedrock handoff; SBM itself tests duration on 20 Hz client ticks.
            revealAt = putAwayEnd + ExitGuardMs;
            return Event::PutAway;
        }
        reveal(now);
        return Event::Reveal;
    }
    Event advance(int64_t now) {
        if (!puttingAway || now < revealAt) return Event::None;
        reveal(now);
        return Event::Reveal;
    }
    bool blocked(int64_t now) const { return puttingAway || now < drawUntil; }
    void clear() { *this = {}; }

    HandSelection selected;
    HandSelection visible;
    bool puttingAway = false;
    int64_t putAwayEnd = 0;
    int64_t revealAt = 0;
    int64_t drawUntil = 0;

private:
    int64_t pendingDraw = 0;
    void reveal(int64_t now) {
        visible = selected;
        puttingAway = false;
        drawUntil = now + (visible.gun ? pendingDraw : 0);
    }
};

} // namespace tacz::gun
