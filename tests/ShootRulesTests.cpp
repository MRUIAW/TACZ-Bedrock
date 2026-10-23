#include <doctest/doctest.h>

#include <random>

#include "gun/ShootRules.h"
#include "pack/GunPackParser.h"

using namespace tacz;
using gun::GunAmmo;
using pack::Bolt;
using pack::FireMode;

namespace {

// Fires until the gun refuses; returns the number of rounds that left the barrel.
int emptyGun(Bolt bolt, GunAmmo ammo) {
    int fired = 0;
    while (gun::prepareShot(bolt, ammo) == gun::ShotCheck::Ok && gun::reduceAmmoOnce(bolt, ammo)) ++fired;
    return fired;
}

} // namespace

TEST_CASE("Fire mode cycles through the gun's list; unknown goes to the first") {
    std::vector<std::optional<FireMode>> modes{FireMode::Auto, FireMode::Semi};
    CHECK(gun::nextFireMode(modes, FireMode::Auto) == FireMode::Semi);
    CHECK(gun::nextFireMode(modes, FireMode::Semi) == FireMode::Auto);
    CHECK(gun::nextFireMode(modes, std::nullopt) == FireMode::Auto);
    CHECK(gun::nextFireMode(modes, FireMode::Burst) == FireMode::Auto);
}

TEST_CASE("Closed bolt: magazine plus chamber, chambering takes one from the magazine") {
    CHECK(emptyGun(Bolt::ClosedBolt, {30, true}) == 31);
    CHECK(emptyGun(Bolt::ClosedBolt, {30, false}) == 30);

    GunAmmo ammo{30, false};
    REQUIRE(gun::prepareShot(Bolt::ClosedBolt, ammo) == gun::ShotCheck::Ok);
    CHECK(ammo == GunAmmo{29, true});
    REQUIRE(gun::reduceAmmoOnce(Bolt::ClosedBolt, ammo));
    CHECK(ammo == GunAmmo{28, true});

    GunAmmo last{0, true};
    REQUIRE(gun::reduceAmmoOnce(Bolt::ClosedBolt, last));
    CHECK(last == GunAmmo{0, false});
    CHECK(gun::prepareShot(Bolt::ClosedBolt, last) == gun::ShotCheck::NoAmmo);
}

TEST_CASE("Open bolt ignores the chamber; manual action needs a chambered round each shot") {
    CHECK(emptyGun(Bolt::OpenBolt, {30, true}) == 30);

    GunAmmo bolt{5, true};
    CHECK(emptyGun(Bolt::ManualAction, bolt) == 1);
    GunAmmo afterShot{5, false};
    CHECK(gun::prepareShot(Bolt::ManualAction, afterShot) == gun::ShotCheck::NeedBolt);
}

TEST_CASE("Trigger cooldowns: TACZ server window and client view") {
    auto data = pack::parseGunData(pack::parseJsonText(R"({"rpm": 600, "burst_data": {"min_interval": 0.3, "bpm": 900}})"));
    CHECK(gun::serverCooldownMs(data, FireMode::Auto, 1000, std::nullopt) == 0);
    CHECK(gun::serverCooldownMs(data, FireMode::Auto, 1050, 1000) == 45);
    CHECK(gun::serverCooldownMs(data, FireMode::Auto, 1095, 1000) == 0); // 5 ms window
    CHECK(gun::clientCooldownMs(data, FireMode::Auto, 1095, 1000) == 5);
    CHECK(gun::triggerIntervalMs(data, FireMode::Burst) == 300);
    CHECK(gun::burstShootIntervalMs(data) == 66);
}

TEST_CASE("Bullet speed includes the fire mode adjust and TACZ's global x2 modifier") {
    auto data = pack::parseGunData(pack::parseJsonText(R"({"bullet": {"speed": 250}, "fire_mode_adjust": {"semi": {"speed": 50}}})"));
    CHECK(gun::bulletSpeedPerTick(data, FireMode::Auto) == doctest::Approx(25.0));
    CHECK(gun::bulletSpeedPerTick(data, FireMode::Semi) == doctest::Approx(30.0));
}

TEST_CASE("Continuous fire: auto always, burst only with continuous_shoot") {
    auto data = pack::parseGunData(pack::parseJsonText(R"({"burst_data": {"continuous_shoot": false}})"));
    CHECK(gun::firesContinuously(data, FireMode::Auto));
    CHECK_FALSE(gun::firesContinuously(data, FireMode::Semi));
    CHECK_FALSE(gun::firesContinuously(data, FireMode::Burst));
    CHECK_FALSE(gun::firesContinuously(data, std::nullopt));
    data.burst.continuousShoot = true;
    CHECK(gun::firesContinuously(data, FireMode::Burst));
}

TEST_CASE("Inaccuracy: state selection and fire mode addend") {
    using pack::InaccuracyType;
    CHECK(gun::inaccuracyType(true, true, 1.0) == InaccuracyType::Aim);
    CHECK(gun::inaccuracyType(false, true, 1.0) == InaccuracyType::Sneak);
    CHECK(gun::inaccuracyType(false, false, 0.1) == InaccuracyType::Move);
    CHECK(gun::inaccuracyType(false, false, 0.01) == InaccuracyType::Stand);

    auto data = pack::parseGunData(pack::parseJsonText(R"({"inaccuracy": {"stand": 4.5, "aim": 0.15},
        "fire_mode_adjust": {"semi": {"aim_inaccuracy": -0.5, "other_inaccuracy": -0.5}}})"));
    CHECK(gun::inaccuracy(data, FireMode::Auto, InaccuracyType::Stand) == doctest::Approx(4.5));
    CHECK(gun::inaccuracy(data, FireMode::Semi, InaccuracyType::Stand) == doctest::Approx(4.0));
    CHECK(gun::inaccuracy(data, FireMode::Semi, InaccuracyType::Aim) == doctest::Approx(0.0)); // clamped
}

TEST_CASE("Spread: none at zero inaccuracy, bounded triangular jitter otherwise") {
    std::mt19937 rng(42);
    auto exact = gun::spreadVelocity({0, 0, 2}, 12.5, 0.0f, rng);
    CHECK(exact.z == doctest::Approx(12.5));
    CHECK(exact.x == doctest::Approx(0));

    double maxOffset = 0.0172275 * 5.0;
    for (int i = 0; i < 1000; ++i) {
        auto v = gun::spreadVelocity({0, 0, 1}, 1.0, 5.0f, rng);
        CHECK(std::abs(v.x) <= maxOffset);
        CHECK(std::abs(v.y) <= maxOffset);
        CHECK(std::abs(v.z - 1.0) <= maxOffset);
    }
}
