#pragma once

#include <cstdint>
#include <optional>

namespace tacz::gun {

// Frame-owned gate; the adapter supplies a counter from real client ticks via
// an atomic. Neither input release nor another attempt rearms the same tick.
class ClientTickGate {
    std::optional<uint64_t> lastTick;
public:
    bool tryAcquire(uint64_t tick) {
        if (lastTick && *lastTick == tick) return false;
        lastTick = tick;
        return true;
    }
    void reset() { lastTick.reset(); }
};

} // namespace tacz::gun
