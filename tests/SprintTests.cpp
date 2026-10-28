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

TEST_CASE("Held-fire sprint intent publishes changes only including release") {
    gun::ShootSprintIntent intent;
    CHECK_FALSE(intent.observe({11, 0, true}));
    CHECK(intent.update(true) == std::optional{true});
    for (int frame = 0; frame < 100; ++frame) {
        CHECK_FALSE(intent.observe({11, 0, true}));
        CHECK_FALSE(intent.update(true));
        CHECK(intent.held);
    }
    CHECK(intent.update(false) == std::optional{false});
    CHECK_FALSE(intent.update(false));
    CHECK_FALSE(intent.held);
}

TEST_CASE("Server held-fire intent is ineligible without a living nonspectator main-hand gun") {
    for (bool alive : {false, true}) for (bool spectator : {false, true}) for (bool gunHeld : {false, true}) {
        gun::ShootSprintIntent intent;
        intent.observe({22, 0, alive && !spectator && gunHeld});
        intent.update(true);
        CHECK(intent.held == (alive && !spectator && gunHeld));
        intent.update(false); // release is always accepted, even when ineligible
        CHECK_FALSE(intent.held);
    }
}

TEST_CASE("Sprint intent clears across selection life dimension actor and world boundaries") {
    for (auto next : {gun::SprintIntentScope{11, 0, true}, {12, 0, true}, {11, 1, true}, {11, 0, false}}) {
        gun::ShootSprintIntent intent;
        intent.observe({11, 0, true});
        intent.update(true);
        CHECK(intent.observe(next, next == gun::SprintIntentScope{11, 0, true}) == std::optional{false});
        CHECK_FALSE(intent.held);
        CHECK_FALSE(intent.observe(next));
        // Respawn/re-entry does not resurrect a previous press.
        intent.observe({11, 0, true});
        CHECK_FALSE(intent.held);
        intent.update(true);
        intent = {}; // server disconnect/world stop and client world generation cleanup
        CHECK_FALSE(intent.held);
        CHECK_FALSE(intent.scope);
        CHECK_FALSE(intent.update(true));
    }
}

TEST_CASE("Switching guns while held releases old intent then publishes the new selection intent") {
    gun::ShootSprintIntent client, server;
    client.observe({11, 0, true}); server.observe({22, 0, true});
    auto deliver = [&](std::optional<bool> change) { if (change) server.update(*change); };
    deliver(client.update(true));
    REQUIRE(server.held);
    server.observe({22, 0, true}, true); // server observes its selected stack, not a client gun id
    CHECK_FALSE(server.held);
    auto release = client.observe({11, 0, true}, true);
    CHECK(release == std::optional{false});
    deliver(release);
    deliver(client.update(true));
    CHECK(server.held);
}

TEST_CASE("Held-fire ADS and reload share one sprint block policy without clearing each other") {
    using R = gun::ReloadStateType;
    for (bool intent : {false, true}) for (bool aim : {false, true}) {
        for (auto reload : {R::NotReloading, R::EmptyFeeding, R::EmptyFinishing, R::TacticalFeeding, R::TacticalFinishing}) {
            CHECK(gun::blocksSprint(intent, aim, reload)
                == (intent || aim || reload == R::EmptyFeeding || reload == R::TacticalFeeding));
        }
    }
    CHECK(gun::blocksSprint(true, false, R::NotReloading)); // ADS release while trigger stays held
    CHECK(gun::blocksSprint(false, true, R::NotReloading)); // trigger release while ADS stays held
    CHECK_FALSE(gun::blocksSprint(false, false, R::EmptyFinishing));
}

TEST_CASE("Stop intent breaks the latched-server sprint deadlock and permits one held semi-auto shot") {
    // Synthetic native boundary model, not an assertion about internal Bedrock packet timing:
    // stripping starts alone leaves an already-sprinting server with no explicit stop.
    bool nativeSprint = true;
    float authoritative = .2f;
    authoritative = gun::advanceSprintTime(authoritative, .1f, .2f, nativeSprint, false);
    CHECK(authoritative == .2f);
    gun::ShootSprintIntent client, server;
    client.observe({11, 0, true}); server.observe({22, 0, true});
    auto change = client.update(gun::shootStopsSprint(true, true, false, true));
    REQUIRE(change);
    server.update(*change);
    int shots = 0;
    bool lastSuccess = false;
    for (int tick = 0; tick < 8; ++tick) {
        bool blocked = gun::blocksSprint(server.held, false, gun::ReloadStateType::NotReloading);
        if (blocked) nativeSprint = false; // existing setSprinting(false)
        nativeSprint = !blocked; // existing native re-entry hook refuses a held sprint request
        CHECK_FALSE(nativeSprint);
        authoritative = gun::advanceSprintTime(authoritative, .0625f, .2f, nativeSprint, false);
        if (tick < 3) CHECK(gun::sprintBlocksShot(authoritative));
        else CHECK(authoritative == 0);
        for (int frame = 0; frame < 3; ++frame) {
            if (!lastSuccess && !gun::sprintBlocksShot(authoritative)) { ++shots; lastSuccess = true; }
        }
    }
    CHECK(shots == 1);
    server.update(false);
    CHECK_FALSE(gun::blocksSprint(server.held, false, gun::ReloadStateType::NotReloading));
}
