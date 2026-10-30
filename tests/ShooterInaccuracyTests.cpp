#include <doctest/doctest.h>
#include "gun/ShooterInaccuracy.h"

using namespace tacz;

namespace {
pack::GunData fixtureData() {
    pack::GunData data;
    data.inaccuracy = std::map<pack::InaccuracyType, float>{
        {pack::InaccuracyType::Stand, 4.5f}, {pack::InaccuracyType::Move, 5.0f},
        {pack::InaccuracyType::Sneak, 3.5f}, {pack::InaccuracyType::Aim, 0.15f}};
    return data;
}
gun::RoundInaccuracy select(gun::AuthInputRing const& input, bool aim = false, bool sneak = false) {
    return gun::authInputInaccuracy(input, fixtureData(), pack::FireMode::Semi, aim, sneak);
}
}

TEST_CASE("Identical received positions select STAND despite native ground gravity residue") {
    gun::AuthInputRing input;
    input.receive({{3, 1.62, 4}});
    input.receive({{3, 1.62, 4}});
    auto result = select(input);
    CHECK(result.samples == 2);
    CHECK(result.movement == glm::dvec3{0});
    CHECK(result.type == pack::InaccuracyType::Stand);
    CHECK(result.value == 4.5f);
    // The old input explains the bug; it is not an input to the new selector.
    CHECK(gun::inaccuracyType(false, false, glm::length(glm::dvec3{0, -0.0784, 0})) == pack::InaccuracyType::Move);
}

TEST_CASE("A received horizontal step selects MOVE from the full position difference") {
    gun::AuthInputRing input;
    input.receive({{0, 1.62, 0}});
    input.receive({{0.2, 1.62, 0}});
    auto result = select(input);
    CHECK(result.samples == 2);
    CHECK(result.movement == glm::dvec3{0.2, 0, 0});
    CHECK(result.type == pack::InaccuracyType::Move);
    CHECK(result.value == 5.0f);
}

TEST_CASE("A received Y-only jump selects MOVE without excluding vertical displacement") {
    gun::AuthInputRing input;
    input.receive({{0, 1.62, 0}});
    input.receive({{0, 1.82, 0}});
    auto result = select(input);
    CHECK(result.movement.x == 0);
    CHECK(result.movement.y == doctest::Approx(0.2));
    CHECK(result.movement.z == 0);
    CHECK(result.type == pack::InaccuracyType::Move);
}

TEST_CASE("Missing single and cleared auth records use zero with truthful sample counts") {
    gun::AuthInputRing input;
    auto checkZero = [&](unsigned samples) {
        auto result = select(input);
        CHECK(result.samples == samples);
        CHECK(result.movement == glm::dvec3{0});
        CHECK(result.type == pack::InaccuracyType::Stand);
    };
    checkZero(0);
    input.bind(1, 2, 0);
    input.receive({{100, 200, 300}});
    checkZero(1);
    input.receive({{101, 201, 301}});
    REQUIRE(select(input).type == pack::InaccuracyType::Move);
    input.clear(); // disconnect/death/respawn erase the native record
    checkZero(0);
    input.receive({{500, 500, 500}});
    checkZero(1);
    input.receive({{501, 501, 501}});
    input.bind(1, 2, 1); // dimension transition
    checkZero(0);
    input.receive({{1, 2, 3}});
    input.bind(3, 2, 1); // world replacement
    checkZero(0);
}

TEST_CASE("Aim and sneak precede auth displacement and the move threshold remains strictly 0.05") {
    gun::AuthInputRing input;
    input.receive({{0, 0, 0}});
    input.receive({{0.05, 0, 0}});
    CHECK(select(input).type == pack::InaccuracyType::Stand);
    input.receive({{0.100001, 0, 0}});
    CHECK(select(input).type == pack::InaccuracyType::Move);
    auto aimed = select(input, true, true);
    CHECK(aimed.type == pack::InaccuracyType::Aim);
    CHECK(aimed.value == 0.15f);
    auto sneaking = select(input, false, true);
    CHECK(sneaking.type == pack::InaccuracyType::Sneak);
    CHECK(sneaking.value == 3.5f);
    input.clear();
    CHECK(select(input, true, true).type == pack::InaccuracyType::Aim);
    CHECK(select(input, false, true).type == pack::InaccuracyType::Sneak);
}

TEST_CASE("Receipt displacement is not sorted filtered or normalized by client or receipt ticks") {
    gun::AuthInputRing input;
    input.receive({{0, 0, 0}, {}, 0, 100, 50});
    input.receive({{0.2, 0, 0}, {}, 0, 99, 50}); // same server tick, descending client tick
    CHECK(select(input).movement.x == 0.2);
    input.receive({{0.5, 0, 0}, {}, 0, 110, 60}); // gap is not divided out
    CHECK(select(input).movement.x == doctest::Approx(0.3));
    CHECK(select(input).samples == 2);
}

TEST_CASE("Prepared inaccuracy owns movement type count and spread across delayed burst callbacks") {
    gun::AuthInputRing input;
    input.receive({{0, 1.62, 0}});
    input.receive({{0, 1.62, 0}});
    auto prepared = select(input);
    auto cycle = [prepared] { return prepared; };
    input.receive({{0.2, 1.62, 0}});
    for (int round = 0; round < 3; ++round) {
        CHECK(cycle().movement == glm::dvec3{0});
        CHECK(cycle().samples == 2);
        CHECK(cycle().type == pack::InaccuracyType::Stand);
        CHECK(cycle().value == 4.5f);
    }
    // A later shootOnce prepares afresh, not a later pellet of the old cycle.
    CHECK(select(input).type == pack::InaccuracyType::Move);
    CHECK(select(input).value == 5.0f);
    CHECK(gun::inaccuracyTypeLabel(pack::InaccuracyType::Stand) == "STAND");
    CHECK(gun::inaccuracyTypeLabel(pack::InaccuracyType::Move) == "MOVE");
    CHECK(gun::inaccuracyTypeLabel(pack::InaccuracyType::Sneak) == "SNEAK");
    CHECK(gun::inaccuracyTypeLabel(pack::InaccuracyType::Aim) == "AIM");
    CHECK(gun::inaccuracyTypeLabel(pack::InaccuracyType::Lie) == "LIE");
}
