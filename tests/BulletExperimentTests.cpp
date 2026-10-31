#include <doctest/doctest.h>
#include "gun/BulletExperiment.h"

using namespace tacz::gun;
namespace {
bullet::Message sample(uint64_t serial = 1) {
    bullet::Message m; m.serial = serial; m.runtime = serial + 100; m.unique = -55;
    m.dimension = 7; m.motion.pos = {1.25,64.125,-9}; m.motion.vel = {2,-.5,3};
    m.motion.gravity = .1F; m.motion.friction = .02F; m.motion.life = 100;
    return m;
}
}
TEST_CASE("Bullet experiment spawn/removal codec is exact, finite and bounded") {
    for (auto kind : {bullet::Kind::Spawn,bullet::Kind::Remove}) {
        auto m = sample(); m.kind = kind;
        auto bytes = bullet::encode(m); CHECK(bytes.size() == bullet::WireSize);
        auto decoded = bullet::decode(bytes); REQUIRE(decoded);
        CHECK(decoded->kind == kind); CHECK(decoded->serial == m.serial);
        CHECK(decoded->runtime == m.runtime); CHECK(decoded->unique == m.unique);
        CHECK(decoded->dimension == m.dimension); CHECK(decoded->motion.pos == m.motion.pos);
        CHECK(decoded->motion.vel == m.motion.vel); CHECK(decoded->motion.gravity == m.motion.gravity);
        CHECK(decoded->motion.friction == m.motion.friction); CHECK(decoded->motion.life == 100);
        CHECK(decoded->waterRules);
        for (size_t n = 0; n < bytes.size(); ++n) CHECK_FALSE(bullet::decode(std::string_view(bytes).substr(0,n)));
        CHECK_FALSE(bullet::decode(bytes + "x"));
        bytes[0] = 2; CHECK_FALSE(bullet::decode(bytes));
    }
    auto m = sample(); m.motion.vel.x = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(bullet::decode(bullet::encode(m)));
    m = sample(); m.motion.gravity = -1; CHECK_FALSE(bullet::decode(bullet::encode(m)));
    m = sample(); m.motion.life = 0; CHECK_FALSE(bullet::decode(bullet::encode(m)));
    m = sample(); m.runtime = 0; CHECK_FALSE(bullet::decode(bullet::encode(m)));
}
TEST_CASE("Client and server share move then air/water drag and gravity") {
    auto m = sample(); auto server = m.motion; bullet::State client(m);
    for (int tick = 0; tick < 40; ++tick) {
        bool water = tick >= 3 && tick < 15;
        auto old = server;
        CHECK(server.step(water) == client.step(water));
        CHECK(client.previous == old.pos);
        CHECK(client.motion.pos == server.pos); CHECK(client.motion.vel == server.vel);
        auto friction = water ? .4F : old.friction;
        auto gravity = water ? old.gravity * .6F : old.gravity;
        CHECK(server.pos == old.pos + old.vel);
        auto oracle = old.vel * static_cast<double>(1.0F-friction);
        oracle.y -= gravity;
        CHECK(server.vel == oracle);
    }
    m.waterRules = false; bullet::State dry(m); auto expected = m.motion;
    dry.step(true); expected.step(false); CHECK(dry.motion.vel == expected.vel);
}
TEST_CASE("Every five server placements include spawn and any removal endpoint") {
    for (int age = 0; age <= 23; ++age) {
        CHECK(bullet::placeDue(bullet::SyncMode::Tick,age));
        CHECK(bullet::placeDue(bullet::SyncMode::Every5,age) == (age % 5 == 0));
        CHECK(bullet::placeDue(bullet::SyncMode::Every5,age,true));
    }
}
TEST_CASE("Movement filter uses bullet age, never packet count") {
    for (int age = 0; age < 20; ++age) {
        CHECK(bullet::acceptMovement(bullet::DriveMode::Off,age));
        CHECK(bullet::acceptMovement(bullet::DriveMode::Drive,age));
        for (int packet = 0; packet < 3; ++packet)
            CHECK(bullet::acceptMovement(bullet::DriveMode::Filter,age) == (age > 0 && age % 5 == 0));
    }
}
TEST_CASE("Java bullet corrections snap current state without blending or changing velocity/previous") {
    auto m = sample(); bullet::State client(m); client.step(false);
    auto oldPrevious = client.previous, oldVelocity = client.motion.vel;
    glm::dvec3 target{5,12,-3};
    auto distance = glm::length(target-client.motion.pos);
    CHECK(client.snap(target) == doctest::Approx(distance));
    CHECK(client.motion.pos == target); CHECK(client.motion.vel == oldVelocity);
    CHECK(client.previous == oldPrevious);
    CHECK(bullet::interpolate(client.previous,client.motion.pos,0) == oldPrevious);
    CHECK(bullet::interpolate(client.previous,client.motion.pos,1) == target);
    CHECK(bullet::interpolate(client.previous,client.motion.pos,.5) == (oldPrevious+target)*.5);
}
TEST_CASE("Filtered partial-axis packets still update the last received baseline") {
    auto m = sample(); bullet::State client(m);
    client.step(false); auto before = client.motion.pos, velocity = client.motion.vel;
    auto rejected = client.receive(bullet::DriveMode::Filter,1,{12,0,0},{true,false,false});
    CHECK(rejected.filtered); CHECK_FALSE(rejected.snap);
    CHECK(client.motion.pos == before); CHECK(client.lastServer.x == 12);
    client.motion.age = 5;
    auto accepted = client.receive(bullet::DriveMode::Filter,1,{0,80,0},{false,true,false});
    CHECK_FALSE(accepted.filtered); REQUIRE(accepted.snap);
    CHECK(client.motion.pos == glm::dvec3(12,80,m.motion.pos.z));
    CHECK(client.motion.vel == velocity);
    auto position = client.motion.pos;
    CHECK_FALSE(client.receive(bullet::DriveMode::Filter,3,{9,9,9},{}).snap);
    CHECK_FALSE(client.receive(bullet::DriveMode::Filter,2,{9,9,9},{}).snap);
    CHECK(client.motion.pos == position); CHECK(client.motion.vel == velocity);
    CHECK(client.received == 4); CHECK(client.filtered == 1);
    CHECK(client.delta == 2); CHECK(client.hints == 1); CHECK(client.nativeMotion == 1);
}
TEST_CASE("Unknown/late bullet messages, expiry, and lifecycle clear are safe without a bullet cap") {
    bullet::States states;
    CHECK_FALSE(states.correct(99,{1,2,3})); CHECK_FALSE(states.remove(99));
    auto m = sample(); m.motion.life = 3; REQUIRE(states.spawn(m));
    CHECK_FALSE(states.spawn(m)); REQUIRE(states.runtime(m.runtime));
    CHECK(states.entries.at(m.serial).step(false));
    CHECK_FALSE(states.entries.at(m.serial).step(false));
    CHECK(states.remove(m.serial)); CHECK_FALSE(states.runtime(m.runtime));
    CHECK_FALSE(states.spawn(m)); CHECK_FALSE(states.correct(m.serial,{0,0,0}));
    for (uint64_t n = 2; n < 1026; ++n) CHECK(states.spawn(sample(n)));
    CHECK(states.entries.size() == 1024);
    states.clear(); CHECK(states.entries.empty()); CHECK_FALSE(states.runtime(102));
    CHECK(states.spawn(m)); // new world may reuse protocol serials
}
TEST_CASE("Bullet diagnostic arming is bounded independently of active bullets") {
    bullet::DiagnosticBudget budget; budget.arm(100,10);
    for (int n = 0; n < 16; ++n) CHECK(budget.take(20));
    CHECK_FALSE(budget.take(20)); budget.arm(1,20); CHECK_FALSE(budget.take(420));
    budget.arm(0,20); CHECK_FALSE(budget.take(20));
}
