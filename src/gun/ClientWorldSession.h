#pragma once

#include <atomic>
#include <cstdint>

namespace tacz::gun {

// A LocalPlayer pointer may exist before onPlayerLoaded or during onLevelExit. Only an
// explicitly opened world accepts work. Tokens invalidate queues across leave/rejoin,
// even if the engine reuses the same player/level/sound-engine address.
class ClientWorldSession {
public:
    uint64_t open() {
        auto previous = state.load();
        uint64_t next;
        do { next = (previous + 2) | 1; } while (!state.compare_exchange_weak(previous, next));
        return next;
    }
    void close() { state.fetch_and(~uint64_t{1}); }
    uint64_t current() const {
        auto value = state.load();
        return value & 1 ? value : 0;
    }
    bool accepts(uint64_t token) const { return token != 0 && current() == token; }

private:
    std::atomic<uint64_t> state{0};
};

} // namespace tacz::gun
