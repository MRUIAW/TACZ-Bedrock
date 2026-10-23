#include <doctest/doctest.h>

#include "gun/BulletPhysics.h"

using namespace tacz;

TEST_CASE("Bullet launch converts m/s to blocks per tick and life to ticks") {
    pack::BulletData data;
    data.speed    = 250;
    data.gravity  = 0.15f;
    data.friction = 0.02f;
    data.life     = 0.8f;
    auto m        = gun::BulletMotion::launch(data, {0, 64, 0}, {0, 0, 2});
    CHECK(m.vel.z == doctest::Approx(12.5));
    CHECK(m.vel.x == doctest::Approx(0));
    CHECK(m.life == 16);
}

TEST_CASE("Bullet step: move, then drag and gravity; expires at life - 1 (TACZ order)") {
    pack::BulletData data;
    data.speed    = 20; // 1 block per tick
    data.gravity  = 0.1f;
    data.friction = 0.5f;
    data.life     = 0.2f; // 4 ticks
    auto m        = gun::BulletMotion::launch(data, {0, 0, 0}, {1, 0, 0});

    CHECK(m.next().x == doctest::Approx(1));
    CHECK(m.step());
    CHECK(m.pos.x == doctest::Approx(1));
    CHECK(m.vel.x == doctest::Approx(0.5));
    CHECK(m.vel.y == doctest::Approx(-0.1));

    CHECK(m.step());       // age 2
    CHECK_FALSE(m.step()); // age 3 >= life - 1
    CHECK(m.pos.x == doctest::Approx(1 + 0.5 + 0.25));
}

TEST_CASE("Bullet life never drops below one tick") {
    pack::BulletData data;
    data.life = 0;
    auto m    = gun::BulletMotion::launch(data, {0, 0, 0}, {0, 0, 1});
    CHECK(m.life == 1);
    CHECK_FALSE(m.step());
}

TEST_CASE("Bullet rotation follows Bedrock yaw/pitch conventions") {
    gun::BulletMotion m;
    m.vel = {0, 0, 1};
    CHECK(m.rotation().y == doctest::Approx(0));
    m.vel = {-1, 0, 0};
    CHECK(m.rotation().y == doctest::Approx(90));
    m.vel = {0, -1, 1};
    CHECK(m.rotation().x == doctest::Approx(45)); // looking down is positive pitch
}
