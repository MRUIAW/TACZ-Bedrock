#include <doctest/doctest.h>
#include "gun/HitInput.h"
#include "gun/BulletHit.h"

using namespace tacz;

TEST_CASE("Received auth ring retains the latest two in receipt order and invalidates across life world actor dimension") {
    gun::AuthInputRing ring;
    CHECK(gun::authInputFeet({3, 1.62, 4}) == glm::dvec3{3, 0, 4});
    ring.bind(1, 2, 0);
    CHECK_FALSE(ring.latest); CHECK_FALSE(ring.previous);
    gun::AuthInput a{{1, 2, 3}, {4, 5}, 6, 10, 20}, b{{7, 8, 9}, {10, 11}, 12, 11, 21};
    ring.receive(a); CHECK(ring.latest->clientTick == 10); CHECK_FALSE(ring.previous);
    ring.receive(b); CHECK(ring.latest->pos == b.pos); CHECK(ring.previous->pos == a.pos);
    auto c = a; c.clientTick = 9;
    ring.receive(c); CHECK(ring.latest->clientTick == 9); CHECK(ring.previous->clientTick == 11);
    ring.bind(1, 2, 0); REQUIRE(ring.latest); CHECK(ring.latest->receiptTick == 20);
    // Native disconnect/death/respawn callbacks erase the ring; clear has the same empty semantics.
    for (int event = 0; event < 3; ++event) {
        ring.clear(); CHECK_FALSE(ring.latest); CHECK_FALSE(ring.previous); ring.receive(a);
    }
    ring.bind(1, 2, 1); CHECK_FALSE(ring.latest); CHECK_FALSE(ring.previous);
    ring.receive(a); ring.bind(3, 2, 1); CHECK_FALSE(ring.latest);
    ring.receive(a); ring.bind(3, 4, 1); CHECK_FALSE(ring.latest);
}

TEST_CASE("Round origin converts Bedrock base anchor to feet midpoint then Java pose eye height with current fallback") {
    gun::AuthInputRing ring;
    auto aim = gun::roundAim(ring, {3, -60, 4}, {0, 0}, 1.62);
    CHECK(aim.origin == glm::dvec3{3, -58.38, 4}); CHECK(aim.direction == glm::dvec3{0, 0, 1});
    ring.receive({{3, -58.38, 4}, {0, 90}});
    aim = gun::roundAim(ring, {100, 100, 100}, {90, 0}, 1.62);
    CHECK(aim.origin.y == doctest::Approx(-58.38)); CHECK(aim.origin.x == 3);
    ring.receive({{5, -56.38, 8}, {-30, 180}});
    aim = gun::roundAim(ring, {100, 100, 100}, {90, 0}, 1.62);
    CHECK(aim.origin.x == 4); CHECK(aim.origin.y == doctest::Approx(-57.38)); CHECK(aim.origin.z == 6);
    CHECK(gun::javaEyeHeight(false, false, false, false) == 1.62);
    CHECK(gun::javaEyeHeight(true, false, false, false) == 1.27);
    CHECK(gun::javaEyeHeight(true, true, false, false) == 0.4);
    CHECK(gun::javaEyeHeight(false, false, true, false) == 0.4);
    CHECK(gun::javaEyeHeight(false, false, false, true) == 0.4);
    CHECK(gun::roundAim(ring, {}, {}, 1.27).origin.y == doctest::Approx(-57.73));
    CHECK(gun::midpointEyeFromFeet({2, 10, 0}, {4, 12, 0}, 1.62).y == doctest::Approx(12.62));
}

TEST_CASE("Rotation direction is Java pitch yaw including beyond 90 degrees without a clamp") {
    auto forward = gun::directionFromRotation({0, 0}); CHECK(forward == glm::dvec3{0, 0, 1});
    auto left = gun::directionFromRotation({0, 90}); CHECK(left.x == doctest::Approx(-1)); CHECK(left.z == doctest::Approx(0));
    auto down = gun::directionFromRotation({90, 0}); CHECK(down.y == doctest::Approx(-1));
    auto inverted = gun::directionFromRotation({120, 0});
    CHECK(inverted.y == doctest::Approx(-std::sqrt(3.0) / 2)); CHECK(inverted.z == doctest::Approx(-0.5));
    auto up = gun::directionFromRotation({-120, 270});
    CHECK(up.x == doctest::Approx(-0.5)); CHECK(up.y == doctest::Approx(std::sqrt(3.0) / 2));
    CHECK(glm::length(up) == doctest::Approx(1));
    CHECK(gun::directionFromRotation({480, 0}).z == doctest::Approx(-0.5));
}

TEST_CASE("Measured target boxes ignore gravity-only stored motion and track bounded prior phase samples") {
    gun::DisplacementHistory history;
    glm::dvec3 pos{1, 2, 3};
    CHECK(history.velocity(1, pos, 0) == glm::dvec3{0});
    history.sample({{1, {pos, 0}}});
    auto velocity = history.velocity(1, pos, 0);
    CHECK(velocity == glm::dvec3{0});
    gun::HitboxTarget target{{{0, 0, 0}, {1, 2, 1}}, velocity, false, false, false};
    auto fixed = gun::fixedHitbox(target);
    CHECK(fixed.min == target.box.min); CHECK(fixed.max == target.box.max);
    target.velocity = {0, -0.0784, 0};
    CHECK(gun::fixedHitbox(target).min.y == doctest::Approx(0.3136));
    CHECK(history.velocity(1, pos + glm::dvec3{0.1, 0, 0}, 0).x == doctest::Approx(0.1));
    CHECK(history.velocity(1, pos, 1) == glm::dvec3{0});
    // Replacement actor gets its own unique id, never the despawned actor's motion.
    CHECK(history.velocity(2, pos + glm::dvec3{100}, 0) == glm::dvec3{0});
    history.sample({{2, {pos, 0}}});
    CHECK(history.velocity(1, pos + glm::dvec3{100}, 0) == glm::dvec3{0});
    CHECK(history.velocity(2, pos + glm::dvec3{1}, 0) == glm::dvec3{1});
    history.sample({}); CHECK(history.velocity(1, pos, 0) == glm::dvec3{0});
    gun::DisplacementHistory::Samples samples;
    for (int i = 0; i < 4100; ++i) samples.emplace(i, gun::DisplacementHistory::Position{pos, 0});
    history.sample(std::move(samples));
    CHECK(history.velocity(4095, pos + glm::dvec3{1}, 0) == glm::dvec3{1});
    CHECK(history.velocity(4096, pos + glm::dvec3{1}, 0) == glm::dvec3{0});
    history.clear(); CHECK(history.velocity(1, pos, 0) == glm::dvec3{0});
}

TEST_CASE("Burst and delayed callbacks sample current received movement at each round not trigger time") {
    gun::AuthInputRing input;
    auto fireRound = [&] { return gun::roundAim(input, {}, {}, 1.62); };
    input.receive({{0, 1.62, 0}, {0, 0}});
    auto first = fireRound();
    // Retained callback captures the provider, never a trigger-time aim snapshot.
    auto delayed = fireRound;
    input.receive({{2, 1.62, 0}, {0, 90}});
    auto second = delayed();
    CHECK(first.origin.x == 0); CHECK(first.direction.z == 1);
    CHECK(second.origin.x == 1); CHECK(second.direction.x == doctest::Approx(-1));
    input.receive({{4, 1.62, 0}, {120, 180}});
    auto third = delayed(); CHECK(third.origin.x == 3); CHECK(third.direction.z == doctest::Approx(0.5));
    input.clear(); auto fallback = delayed(); CHECK(fallback.origin.y == 1.62);
}
