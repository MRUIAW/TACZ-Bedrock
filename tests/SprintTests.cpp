#include <doctest/doctest.h>

#include <limits>
#include "gun/Sprint.h"
#include "model/SprintInput.h"

using namespace tacz;

TEST_CASE("Sprint recovery grows by elapsed seconds and caps at the held gun limit") {
    float time = 0;
    time = gun::advanceSprintTime(time, .037f, .2f, true, false);
    CHECK(time == doctest::Approx(.037f));
    time = gun::advanceSprintTime(time, .081f, .2f, true, false);
    CHECK(time == doctest::Approx(.118f));
    CHECK(gun::advanceSprintTime(time, 2.f, .2f, true, false) == .2f);
    CHECK(gun::advanceSprintTime(.2f, .05f, .4f, true, false) == doctest::Approx(.25f));
    CHECK(gun::advanceSprintTime(.2f, .05f, .1f, true, false) == .1f);
    CHECK(gun::advanceSprintTime(0, .05f, 0.f, true, false) == 0);
}

TEST_CASE("Sprint recovery decays when stopped or crouching and reaches exact zero") {
    for (bool sprinting : {false, true}) {
        float time = gun::advanceSprintTime(.2f, .063f, .2f, sprinting, true);
        CHECK(time == doctest::Approx(.137f));
        CHECK(gun::sprintBlocksShot(time));
        CHECK(gun::advanceSprintTime(time, .2f, .2f, sprinting, true) == 0);
    }
    CHECK(gun::advanceSprintTime(.2f, .05f, .2f, false, false) == doctest::Approx(.15f));
    CHECK(gun::advanceSprintTime(0, 20.f, .2f, false, false) == 0);
    CHECK(gun::advanceSprintTime(.2f, 0, .2f, false, false) == .2f);
    CHECK(gun::advanceSprintTime(.3f, .05f, .1f, false, false) == doctest::Approx(.25f)); // Java caps only on growth
}

TEST_CASE("Missing or unknown held gun clears sprint recovery without elapsed-time dependence") {
    for (bool sprinting : {false, true}) for (bool crouching : {false, true}) {
        CHECK(gun::advanceSprintTime(.2f, 0, std::nullopt, sprinting, crouching) == 0);
        CHECK(gun::advanceSprintTime(.2f, 10.f, std::nullopt, sprinting, crouching) == 0);
    }
}

TEST_CASE("Held shoot sprint suppression is scoped to gameplay main-hand guns and non-spectators") {
    for (bool gameplay : {false, true}) for (bool mainHandGun : {false, true})
    for (bool spectator : {false, true}) for (bool held : {false, true}) {
        CHECK(gun::shootStopsSprint(gameplay, mainHandGun, spectator, held)
            == (gameplay && mainHandGun && !spectator && held));
    }
    // The existing gate retains native hold/toggle intent, also across a dry or
    // already-successful semi-auto held trigger: shot success is not an input.
    model::SprintInputGate gate;
    for (int frame = 0; frame < 10; ++frame) {
        auto decision = gate.sample(gun::shootStopsSprint(true, true, false, true), true, true);
        CHECK_FALSE(decision.down);
        CHECK_FALSE(decision.sprint);
    }
    auto released = gate.sample(gun::shootStopsSprint(true, true, false, false), true, true);
    CHECK(released.down);
    CHECK(released.sprint);
    CHECK(released.resumed);
}

TEST_CASE("Sprint rejection uses positive authoritative time with no epsilon") {
    CHECK_FALSE(gun::sprintBlocksShot(0));
    CHECK(gun::sprintBlocksShot(std::numeric_limits<float>::denorm_min()));
    CHECK(gun::sprintBlocksShot(.2f));
}

TEST_CASE("Held semi-auto retries after sprint recovery without consuming the trigger on rejection") {
    float authoritative = .2f;
    float synced = authoritative;
    bool lastShotSucceeded = false;
    int shots = 0;
    auto clientFrame = [&] {
        if (!lastShotSucceeded && !gun::sprintBlocksShot(synced)) {
            lastShotSucceeded = true;
            ++shots;
        }
    };
    for (int tick = 0; tick < 4; ++tick) {
        clientFrame();
        CHECK_FALSE(lastShotSucceeded);
        authoritative = gun::advanceSprintTime(authoritative, .0625f, .2f, false, false);
        // Multiple render frames never decay the synced value on their own.
        clientFrame(); clientFrame();
        CHECK(shots == 0);
        synced = authoritative;
    }
    CHECK(authoritative == 0);
    clientFrame();
    CHECK(shots == 1);
    clientFrame();
    CHECK(shots == 1); // still held, semi-auto only fires once
}
