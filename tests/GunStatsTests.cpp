#include <doctest/doctest.h>

#include "gun/GunStats.h"
#include "pack/GunPackParser.h"

using namespace tacz;

TEST_CASE("Rate of fire: fire mode adjust, invalid values and clamping") {
    auto data = pack::parseGunData(pack::parseJsonText(R"({"rpm": 600, "fire_mode_adjust": {"semi": {"rpm": -700}}})"));
    CHECK(gun::roundsPerMinute(data, pack::FireMode::Auto) == 600);
    CHECK(gun::roundsPerMinute(data, pack::FireMode::Semi) == 300); // <= 0 falls back to 300
    data.rpm = 5000;
    CHECK(gun::shootIntervalMs(data, pack::FireMode::Auto) == 50); // clamped to 1200 rpm
    data.burst.bpm = 0;
    CHECK(gun::burstShootIntervalMs(data) == 300);
}

TEST_CASE("Damage without extra_damage uses bullet.damage at any range, headshot is 0") {
    auto data = pack::parseGunData(pack::parseJsonText(R"({"bullet": {"damage": 4}})"));
    auto dmg  = gun::BulletDamage::from(data, pack::FireMode::Auto);
    CHECK(dmg.atDistance(1000) == doctest::Approx(4));
    CHECK(dmg.headShot == doctest::Approx(0));
    CHECK(dmg.armorIgnore == doctest::Approx(0));
}

TEST_CASE("Damage table with fire mode adjust and config multiplier") {
    auto data = pack::parseGunData(pack::parseJsonText(R"({
        "bullet": {"extra_damage": {"armor_ignore": 0.9, "head_shot_multiplier": 2,
            "damage_adjust": [{"distance": 10, "damage": 5}, {"distance": "infinite", "damage": 2}]}},
        "fire_mode_adjust": {"semi": {"damage": 1, "armor_ignore": 0.5, "head_shot_multiplier": 0.5}}
    })"));
    gun::DamageConfig config{2, 1, 1};
    auto dmg = gun::BulletDamage::from(data, pack::FireMode::Semi, config);
    CHECK(dmg.atDistance(5) == doctest::Approx(12));  // (5 + 1) * 2
    CHECK(dmg.atDistance(50) == doctest::Approx(6));  // (2 + 1) * 2
    CHECK(dmg.headShot == doctest::Approx(2.5));
    CHECK(dmg.armorIgnore == doctest::Approx(1));     // 1.4 clamped
}

TEST_CASE("Empty damage_adjust deals no damage (TACZ parity)") {
    auto data = pack::parseGunData(pack::parseJsonText(R"({"bullet": {"damage": 4, "extra_damage": {}}})"));
    auto dmg  = gun::BulletDamage::from(data, pack::FireMode::Auto);
    CHECK(dmg.atDistance(1) == 0);
}
