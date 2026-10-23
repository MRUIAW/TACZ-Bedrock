#include <doctest/doctest.h>

#include "gun/BulletHit.h"

using namespace tacz;

namespace {
gun::Box unitBox() { return {{0, 0, 0}, {1, 2, 1}}; }
} // namespace

TEST_CASE("Hit clip returns the entry point, misses outside the segment") {
    auto box = unitBox();
    auto hit = gun::clip(box, {-1, 1, 0.5}, {2, 1, 0.5});
    REQUIRE(hit);
    CHECK(hit->x == doctest::Approx(0));
    CHECK(hit->y == doctest::Approx(1));

    CHECK_FALSE(gun::clip(box, {-3, 1, 0.5}, {-0.5, 1, 0.5})); // stops short
    CHECK_FALSE(gun::clip(box, {-1, 5, 0.5}, {2, 5, 0.5}));    // passes above
    CHECK_FALSE(gun::clip(box, {0.5, 1, 0.5}, {3, 1, 0.5}));   // starts inside (Minecraft AABB.clip)
}

TEST_CASE("Hit clip works on axis-parallel and diagonal rays") {
    auto box = unitBox();
    auto down = gun::clip(box, {0.5, 5, 0.5}, {0.5, -5, 0.5});
    REQUIRE(down);
    CHECK(down->y == doctest::Approx(2));
    auto diag = gun::clip(box, {-1, -1, -1}, {2, 3, 2});
    REQUIRE(diag);
    CHECK(diag->x == doctest::Approx(0));
}

TEST_CASE("Fixed hitbox follows TACZ getFixedBoundingBox") {
    gun::HitboxTarget mob{unitBox(), {0.2, 0, 0}};
    auto box = gun::fixedHitbox(mob);
    // expandTowards(+0.2 x) then move(-5 * 0.2): [0, 1.2] -> [-1, 0.2]
    CHECK(box.min.x == doctest::Approx(-1.0));
    CHECK(box.max.x == doctest::Approx(0.2));
    CHECK(box.max.y == doctest::Approx(2));

    gun::HitboxTarget standing{unitBox(), {0, 0, 0}, true, false, false};
    CHECK(gun::fixedHitbox(standing).max.y == doctest::Approx(2.0625));
    gun::HitboxTarget crouching{unitBox(), {0, 0, 0}, true, true, false};
    CHECK(gun::fixedHitbox(crouching).max.y == doctest::Approx(2.0));

    gun::HitboxTarget runningPlayer{unitBox(), {0.1, 0, 0}, true, false, false};
    // players: +3 offset then -5 => net -2 * velocity
    CHECK(gun::fixedHitbox(runningPlayer).min.x == doctest::Approx(-0.2));
}

TEST_CASE("Headshot band is eye height +- 0.25") {
    CHECK(gun::isHeadshot(1.7, 1.62));
    CHECK(gun::isHeadshot(1.4, 1.62));
    CHECK_FALSE(gun::isHeadshot(1.3, 1.62));
    CHECK_FALSE(gun::isHeadshot(1.9, 1.62));
}
