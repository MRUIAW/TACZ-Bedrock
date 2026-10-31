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
TEST_CASE("Movement filter uses authoritative position age, never packet count or arrival age") {
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
    client.placement(1,glm::vec3(12,m.motion.pos.y,m.motion.pos.z));
    auto rejected = client.receive(bullet::DriveMode::Filter,1,{12,0,0},{true,false,false});
    CHECK(rejected.filtered); CHECK(rejected.positionAge == 1);
    CHECK(client.motion.pos == before); CHECK(client.lastServer.x == 12);
    client.motion.age = 4;
    client.placement(5,glm::vec3(12,80,m.motion.pos.z));
    auto accepted = client.receive(bullet::DriveMode::Filter,1,{0,80,0},{false,true,false});
    CHECK_FALSE(accepted.filtered); CHECK(accepted.positionAge == 5);
    auto nextVelocity = velocity * static_cast<double>(1.F-client.motion.friction);
    nextVelocity.y -= client.motion.gravity;
    client.step(false);
    CHECK(client.motion.pos == glm::dvec3(12,80,m.motion.pos.z));
    CHECK(client.motion.vel == nextVelocity);
    auto position = client.motion.pos;
    client.placement(6,{99,99,99}); // future metadata must not re-age a position-less packet
    CHECK_FALSE(client.receive(bullet::DriveMode::Filter,3,{9,9,9},{}).filtered);
    CHECK_FALSE(client.receive(bullet::DriveMode::Filter,2,{9,9,9},{}).filtered);
    CHECK(client.pending.empty());
    CHECK(client.motion.pos == position); CHECK(client.motion.vel == nextVelocity);
    CHECK(client.received == 4); CHECK(client.filtered == 1);
    CHECK(client.delta == 2); CHECK(client.hints == 1); CHECK(client.nativeMotion == 1);
}
namespace {
struct Interpolator {
    glm::vec3 mPos{-50}; glm::vec2 mRot{-20}; float mHeadYaw = -30;
    int mPositionSteps = 3, mRotationSteps = 2, mHeadYawSteps = 1;
    bool mMustBeCompleted = true, mPlayerControlServerVehicle = false;
};
}
TEST_CASE("Interpolator override replaces targets and all step counts without changing ownership flags") {
    Interpolator native;
    glm::vec3 pos{1,2,3}; glm::vec2 rot{15,-80};
    bullet::overrideInterpolator(native,pos,rot);
    CHECK(native.mPos == pos); CHECK(native.mRot == rot); CHECK(native.mHeadYaw == rot.y);
    CHECK(native.mPositionSteps == 0); CHECK(native.mRotationSteps == 0); CHECK(native.mHeadYawSteps == 0);
    CHECK(native.mMustBeCompleted); CHECK_FALSE(native.mPlayerControlServerVehicle);
}
TEST_CASE("Placement age metadata round trips exact native targets and rejects malformed input") {
    bullet::Placement m{7,101,-55,0,5,glm::vec3{-77.306137F,-59.616280F,-25.724379F}};
    auto bytes = bullet::encodePlacement(m); REQUIRE(bytes.size() == bullet::PlacementWireSize);
    auto decoded = bullet::decodePlacement(bytes); REQUIRE(decoded);
    CHECK(decoded->serial == m.serial); CHECK(decoded->runtime == m.runtime); CHECK(decoded->unique == m.unique);
    CHECK(decoded->dimension == m.dimension); CHECK(decoded->age == m.age); CHECK(decoded->pos == m.pos);
    for (size_t n = 0; n < bytes.size(); ++n) CHECK_FALSE(bullet::decodePlacement(std::string_view(bytes).substr(0,n)));
    CHECK_FALSE(bullet::decodePlacement(bytes+"x")); bytes[0] = 2; CHECK_FALSE(bullet::decodePlacement(bytes));
    m.age = -1; CHECK_FALSE(bullet::decodePlacement(bullet::encodePlacement(m)));
    m.age = 5; m.pos.x = std::numeric_limits<float>::infinity();
    CHECK_FALSE(bullet::decodePlacement(bullet::encodePlacement(m)));
}
TEST_CASE("Server placement ages equal kept native correction ages independent of client phase") {
    for (int positionAge = 1; positionAge <= 30; ++positionAge) {
        CHECK(bullet::placeDue(bullet::SyncMode::Every5,positionAge)
            == bullet::acceptMovement(bullet::DriveMode::Filter,positionAge));
        for (int arrivalAge : {positionAge-1,positionAge,positionAge+1}) {
            auto m = sample(); bullet::State client(m); client.motion.age = arrivalAge;
            glm::vec3 nativeTarget{float(positionAge),float(-positionAge),3.F};
            client.placement(positionAge,nativeTarget);
            auto received = client.receive(bullet::DriveMode::Filter,1,glm::dvec3(nativeTarget),{true,true,true});
            CHECK(received.positionAge == positionAge);
            CHECK(received.filtered == !bullet::placeDue(bullet::SyncMode::Every5,positionAge));
        }
    }
}
TEST_CASE("Age five correction before or after the owning step snaps then overrides in the same tick") {
    auto m = sample(); auto server = m.motion;
    for (int n = 0; n < 5; ++n) server.step(false);
    auto target = glm::vec3(server.pos);
    for (int localAge : {4,5}) {
        bullet::State client(m);
        for (int n = 0; n < localAge; ++n) client.step(false);
        client.placement(5,target);
        auto receipt = client.receive(bullet::DriveMode::Filter,1,glm::dvec3(target),{true,true,true});
        REQUIRE_FALSE(receipt.filtered); REQUIRE(client.pending.size() == 1);
        auto oldVelocity = client.motion.vel;
        bool sampled = false;
        client.tick([&](glm::dvec3 pos) {
            sampled = true;
            if (localAge == 5) CHECK(pos == glm::dvec3(target));
            return false;
        });
        CHECK(sampled); CHECK(client.motion.age == localAge+1); CHECK(client.pending.empty());
        REQUIRE(client.applied.size() == 1);
        CHECK(client.applied[0].afterStep == (localAge == 4));
        CHECK(client.applied[0].snap < .00001);
        auto velocity = oldVelocity * static_cast<double>(1.F-client.motion.friction); velocity.y -= client.motion.gravity;
        CHECK(client.motion.vel == velocity);
        if (localAge == 4) CHECK(client.motion.pos == glm::dvec3(target));
        else CHECK(client.motion.pos == glm::dvec3(target)+oldVelocity);
        Interpolator native;
        auto rot = client.motion.rotation();
        bullet::overrideInterpolator(native,glm::vec3(client.motion.pos),rot);
        CHECK(native.mPos == glm::vec3(client.motion.pos)); CHECK(native.mPositionSteps == 0);
        CHECK(native.mRotationSteps == 0); CHECK(native.mHeadYawSteps == 0);
    }
}
TEST_CASE("Only overwrites without an accepted native server packet are unexplained") {
    bullet::State client(sample()); client.wrote = true; client.lastWritten = {0,0,0};
    CHECK_FALSE(client.observeBefore({0,0,0}));
    client.placement(5,{1,1,1});
    CHECK_FALSE(client.receive(bullet::DriveMode::Drive,1,{1,1,1},{true,true,true}).filtered);
    CHECK(client.observeBefore({1,1,1})); CHECK(client.overwrites == 1); CHECK(client.unexplained == 0);
    client.acceptedTick = 0;
    CHECK(client.observeBefore({1,1,1})); CHECK(client.overwrites == 2); CHECK(client.unexplained == 1);
    CHECK(client.receive(bullet::DriveMode::Filter,1,{9,9,9},{true,true,true}).filtered);
    CHECK(client.unaged == 1); CHECK(client.lastPacketAge == -1);
    CHECK_FALSE(client.receive(bullet::DriveMode::Drive,1,{9,9,9},{true,true,true}).filtered);
    CHECK(client.pending.back().age == -1);
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
