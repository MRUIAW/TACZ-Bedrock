#include <doctest/doctest.h>

#include <limits>
#include "gun/ClientTickGate.h"

using namespace tacz;

TEST_CASE("Reload cancellation is once per real client tick while shooting stays frame driven") {
    gun::ClientTickGate gate;
    int packets = 0, animations = 0, shotAttempts = 0;
    // Synthetic 400 FPS / 20 TPS: packet and animation share one tick gate.
    for (uint64_t tick = 0; tick < 20; ++tick) {
        for (int frame = 0; frame < 20; ++frame) {
            if (gate.tryAcquire(tick)) { ++packets; ++animations; }
            ++shotAttempts; // never inside the cancellation gate
        }
        CHECK(packets == tick + 1);
        CHECK(animations == packets);
    }
    CHECK(packets == 20);
    CHECK(animations == 20);
    CHECK(shotAttempts == 400);
}

TEST_CASE("Reload cancellation tick gate is not a per press latch and does not replay skipped ticks") {
    gun::ClientTickGate gate;
    CHECK(gate.tryAcquire(1));
    CHECK_FALSE(gate.tryAcquire(1)); // release/repress or reload restart in this tick
    CHECK(gate.tryAcquire(2)); // continuously held trigger may cancel next tick
    CHECK_FALSE(gate.tryAcquire(2));
    CHECK(gate.tryAcquire(10)); // missed ticks do not queue extra sends
    for (int frame = 0; frame < 20; ++frame) CHECK_FALSE(gate.tryAcquire(10));
}

TEST_CASE("Reload cancellation tick gate resets per session and accepts counter rollover") {
    gun::ClientTickGate gate;
    CHECK(gate.tryAcquire(0));
    CHECK_FALSE(gate.tryAcquire(0));
    gate.reset();
    CHECK(gate.tryAcquire(0)); // same tick number reused by a new session
    CHECK_FALSE(gate.tryAcquire(0));
    CHECK(gate.tryAcquire(std::numeric_limits<uint64_t>::max()));
    CHECK(gate.tryAcquire(0));
    CHECK_FALSE(gate.tryAcquire(0));
}
