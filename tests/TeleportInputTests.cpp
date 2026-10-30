#include <doctest/doctest.h>
#include "gun/HitInput.h"
#include "gun/ShooterInaccuracy.h"
#include "debug/HitDiagnostic.h"

using namespace tacz;

TEST_CASE("Teleport mark clears shared aim and movement samples and ignores unflagged receipts") {
    gun::AuthInputRing input;
    input.bind(1, 2, 0);
    input.receive({{1, 1.62, 3}});
    input.receive({{2, 1.62, 3}});
    auto marked = input.markTeleport(50, {100, 1.62, 200});
    CHECK(marked.phase == gun::TeleportPhase::Mark);
    CHECK(marked.tick == 50);
    CHECK(marked.ignored == 0);
    CHECK(marked.target == glm::dvec3{100, 1.62, 200});
    CHECK(input.awaitingTeleportAck);
    CHECK_FALSE(input.latest);
    CHECK_FALSE(input.previous);
    CHECK_FALSE(input.receive({{3, 1.62, 3}, {}, 0, 1000, 51}));
    CHECK_FALSE(input.receive({{4, 1.62, 3}, {}, 0, 1001, 52}));
    CHECK_FALSE(input.latest);
    CHECK_FALSE(input.previous);
    CHECK(input.teleportWaitTicks(52) == 2);
    auto accuracy = gun::authInputInaccuracy(input, {}, {}, false, false);
    CHECK(accuracy.samples == 0);
    CHECK(accuracy.movement == glm::dvec3{0});
    CHECK(accuracy.type == pack::InaccuracyType::Stand);
    auto ack = input.receive({{100, 1.62, 200}, {}, 0, 1002, 53}, true);
    REQUIRE(ack);
    CHECK(ack->phase == gun::TeleportPhase::Ack);
    CHECK(ack->ignored == 2);
    CHECK(ack->waitTicks == 3);
    CHECK(ack->target == marked.target);
}

TEST_CASE("Pending teleport shoots from current server feet and rotation not stale input or midpoint") {
    gun::AuthInputRing input;
    input.receive({{-70, 1.62, -70}, {0, 180}});
    input.receive({{-69, 1.62, -69}, {0, 180}});
    input.markTeleport(10, {5, 1.62, 6});
    input.receive({{-68, 1.62, -68}, {0, 180}, 0, 1, 11});
    auto aim = gun::roundAim(input, {5, 0, 6}, {0, 90}, 1.62);
    CHECK(aim.origin == glm::dvec3{5, 1.62, 6});
    CHECK(aim.direction.x == doctest::Approx(-1));
    // Fallback uses current native values, not a frozen mark target/rotation.
    auto changed = gun::roundAim(input, {7, 0, 8}, {30, 0}, 1.27);
    CHECK(changed.origin == glm::dvec3{7, 1.27, 8});
    CHECK(changed.direction.y == doctest::Approx(-0.5));
}

TEST_CASE("Flagged confirmation is the first fresh sample and has no pre teleport midpoint") {
    gun::AuthInputRing input;
    input.receive({{-70, 1.62, -70}});
    input.markTeleport(100, {20, 1.62, 30});
    REQUIRE(input.receive({{20, 1.62, 30}, {0, 0}, 0, 7, 101}, true));
    CHECK_FALSE(input.awaitingTeleportAck);
    REQUIRE(input.latest);
    CHECK_FALSE(input.previous);
    auto aim = gun::roundAim(input, {-100, 0, -100}, {0, 90}, 1.62);
    CHECK(aim.origin == glm::dvec3{20, 1.62, 30});
    CHECK(aim.direction == glm::dvec3{0, 0, 1});
    auto accuracy = gun::authInputInaccuracy(input, {}, {}, false, false);
    CHECK(accuracy.samples == 1);
    CHECK(accuracy.movement == glm::dvec3{0});
    input.receive({{22, 1.62, 30}, {0, 0}, 0, 8, 102});
    CHECK(gun::roundAim(input, {}, {}, 1.62).origin.x == 21);
    CHECK(gun::authInputInaccuracy(input, {}, {}, false, false).movement.x == 2);
}

TEST_CASE("Teleport timeout releases at 100 server ticks without waiting for a packet or a shot") {
    gun::AuthInputRing input;
    input.markTeleport(10, {1, 2, 3});
    input.receive({{0, 0, 0}, {}, 0, 999999, 11}); // client tick does not expire the wait
    CHECK_FALSE(input.expireTeleport(109));
    auto timeout = input.expireTeleport(110); // production calls from the level tick as well
    REQUIRE(timeout);
    CHECK(timeout->phase == gun::TeleportPhase::Timeout);
    CHECK(timeout->tick == 110);
    CHECK(timeout->waitTicks == gun::TeleportAckTimeoutTicks);
    CHECK(timeout->ignored == 1);
    CHECK_FALSE(input.awaitingTeleportAck);
    CHECK_FALSE(input.latest);
    CHECK_FALSE(input.expireTeleport(111)); // one transition, no duplicate timeout lines
    input.receive({{4, 5, 6}, {}, 0, 1, 111});
    REQUIRE(input.latest);
    CHECK(input.latest->pos == glm::dvec3{4, 5, 6});
    CHECK_FALSE(input.previous);
    input.markTeleport(200, {7, 8, 9});
    auto boundary = input.receive({{7, 8, 9}, {}, 0, 2, 300}, true);
    REQUIRE(boundary);
    CHECK(boundary->phase == gun::TeleportPhase::Timeout); // timeout checked before ack
    REQUIRE(input.latest);
    CHECK_FALSE(input.previous);
}

TEST_CASE("Scope and lifecycle clears discard teleport wait deadline and history together") {
    gun::AuthInputRing input;
    input.bind(1, 2, 0);
    auto empty = [&] {
        CHECK_FALSE(input.awaitingTeleportAck);
        CHECK_FALSE(input.latest);
        CHECK_FALSE(input.previous);
        CHECK_FALSE(input.expireTeleport(10000));
    };
    // Native disconnect/death/respawn/server-stop/disable erase the entire record.
    for (int lifecycle = 0; lifecycle < 5; ++lifecycle) {
        input.markTeleport(1, {100, 100, 100});
        input.clear();
        empty();
    }
    input.markTeleport(1, {}); input.bind(1, 2, 1); empty(); // dimension
    input.markTeleport(1, {}); input.bind(3, 2, 1); empty(); // world
    input.markTeleport(1, {}); input.bind(3, 4, 1); empty(); // actor replacement
}

TEST_CASE("Repeated teleports reset the wait but the first ack has no transaction matching") {
    gun::AuthInputRing input;
    input.markTeleport(1, {1, 2, 3});
    input.receive({{}, {}, 0, 0, 2});
    input.markTeleport(3, {4, 5, 6});
    CHECK(input.teleportWaitTicks(3) == 0);
    auto ack = input.receive({{1, 2, 3}, {}, 0, 0, 4}, true);
    REQUIRE(ack);
    CHECK(ack->target == glm::dvec3{4, 5, 6});
    CHECK(ack->ignored == 0);
    CHECK_FALSE(input.awaitingTeleportAck);
    // Accepted known difference: a flag for the earlier target may release the later wait.
    CHECK(input.latest->pos == glm::dvec3{1, 2, 3});
}

TEST_CASE("Teleport diagnostic events can precede shots and share the same hard line budget") {
    debug::hit::Budget budget;
    CHECK_FALSE(budget.eventTag(0));
    budget.arm(1);
    auto mark = budget.eventTag(10);
    REQUIRE(mark);
    CHECK(mark->shot == 0);
    CHECK(budget.take(*mark));
    auto shot = budget.trigger(11, 123);
    REQUIRE(shot); // event did not consume this trigger
    CHECK(shot->shot == 1);
    CHECK(budget.take(*shot));
    for (unsigned line = 2; line < debug::hit::LineLimit; ++line) {
        auto event = budget.eventTag(12);
        REQUIRE(event);
        REQUIRE(budget.take(*event));
    }
    CHECK(budget.used() == debug::hit::LineLimit);
    CHECK_FALSE(budget.eventTag(13));
    budget.arm(0);
    CHECK_FALSE(budget.eventTag(14));
}
