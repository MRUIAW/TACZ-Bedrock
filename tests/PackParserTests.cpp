#include <doctest/doctest.h>

#include <cfloat>

#include "pack/GunPackLoader.h"
#include "pack/GunPackParser.h"

using namespace tacz::pack;

TEST_CASE("ResourceId parses namespaced and bare ids") {
    auto a = ResourceId::parse("tacz:gun/uv/ak47");
    REQUIRE(a);
    CHECK(a->ns == "tacz");
    CHECK(a->path == "gun/uv/ak47");
    CHECK(a->toString() == "tacz:gun/uv/ak47");

    auto b = ResourceId::parse("ak47");
    REQUIRE(b);
    CHECK(b->ns == "minecraft");

    CHECK_FALSE(ResourceId::parse("Tacz:ak47"));
    CHECK_FALSE(ResourceId::parse("tacz:"));
    CHECK_FALSE(ResourceId::parse("tacz:a b"));
}

TEST_CASE("JSON text allows comments and a BOM") {
    auto j = parseJsonText("\xEF\xBB\xBF{\n // line\n \"a\": 1, /* block */ \"b\": [2]\n}");
    CHECK(j["a"] == 1);
    CHECK(j["b"][0] == 2);
    CHECK_THROWS_AS(parseJsonText("{\"a\": }"), ParseError);
}

TEST_CASE("Gun data: defaults match TACZ POJOs") {
    auto data = parseGunData(parseJsonText("{}"));
    CHECK(data.ammoAmount == 30);
    CHECK(data.rpm == 300);
    CHECK(data.bolt == Bolt::OpenBolt);
    CHECK(data.bullet.life == doctest::Approx(10));
    CHECK(data.bullet.friction == doctest::Approx(0.01));
    CHECK(data.bullet.tracerCountInterval == -1);
    CHECK(data.reload.type == FeedType::Magazine);
    CHECK(data.reload.feed.empty == doctest::Approx(2.5));
    REQUIRE(data.fireModes.size() == 1);
    CHECK_FALSE(data.fireModes[0].has_value()); // [UNKNOWN]
}

TEST_CASE("Gun data: special serializers") {
    auto data = parseGunData(parseJsonText(R"({
        "bolt": "closed_bolt",
        "fire_mode": ["auto", "semi", "unknown"],
        "bullet": {
            "ignite": true,
            "extra_damage": {
                "damage_adjust": [{"distance": 30, "damage": 9}, {"distance": "infinite", "damage": 6}]
            },
            "explosion": {"explode": true, "radius": 2}
        },
        "fire_mode_adjust": {"semi": {"rpm": -400}, "bogus": {"rpm": 1}},
        "inaccuracy": {"stand": 4.5, "bogus": 1}
    })"));
    CHECK(data.bolt == Bolt::ClosedBolt);
    REQUIRE(data.fireModes.size() == 3);
    CHECK(data.fireModes[1] == FireMode::Semi);
    CHECK_FALSE(data.fireModes[2].has_value());
    CHECK(data.bullet.ignite.entity);
    CHECK(data.bullet.ignite.block);
    REQUIRE(data.bullet.extraDamage);
    CHECK(data.bullet.extraDamage->headShotMultiplier == doctest::Approx(1));
    REQUIRE(data.bullet.extraDamage->damageAdjust.size() == 2);
    CHECK(data.bullet.extraDamage->damageAdjust[1].distance == FLT_MAX);
    REQUIRE(data.bullet.explosion);
    CHECK(data.bullet.explosion->delay == 0); // no no-arg ctor in TACZ -> zero, not a default
    CHECK(data.fireModeAdjust.size() == 1);
    CHECK(data.fireModeAdjust.at(FireMode::Semi).rpm == -400);
    REQUIRE(data.inaccuracy);
    CHECK(data.inaccuracy->size() == 1);
}

TEST_CASE("Gun display: empty sound ids are skipped, invalid ones fail") {
    auto display = parseGunDisplay(parseJsonText(R"({"sounds": {"shoot": "tacz:ak47/ak47_shoot", "inspect": "",
        "fire_select": "tacz:custom/select"}})"));
    CHECK_FALSE(display.sounds.contains("inspect"));
    CHECK(display.sounds.at("shoot").path == "ak47/ak47_shoot");
    // GunDisplayInstance defaults fill in what the display does not name, and never override it.
    CHECK(display.sounds.at("dry_fire").toString() == "tacz:dry_fire");
    CHECK(display.sounds.at("fire_select").toString() == "tacz:custom/select");
    CHECK_THROWS_AS(parseGunDisplay(parseJsonText(R"({"sounds": {"shoot": "Tacz:Bad"}})")), ParseError);
}

TEST_CASE("Gun data: unknown enum names become unset, wrong types fail") {
    auto data = parseGunData(parseJsonText(R"({"bolt": "pump", "reload": {"type": "belt"}})"));
    CHECK_FALSE(data.bolt.has_value());
    CHECK_FALSE(data.reload.type.has_value());
    CHECK_THROWS_AS(parseGunData(parseJsonText(R"({"rpm": "fast"})")), ParseError);
    CHECK_THROWS_AS(parseGunData(parseJsonText(R"({"bullet": {"extra_damage": {"damage_adjust": [{"damage": 1}]}}})")), ParseError);
}

TEST_CASE("validateGun mirrors CommonGunIndex checks") {
    GunIndexData index;
    index.type = "rifle";
    index.data = ResourceId::parse("tacz:ak47_data");

    auto data   = parseGunData(parseJsonText(R"({"ammo": "tacz:762x39", "bolt": "closed_bolt", "fire_mode": ["auto"]})"));
    auto result = validateGun(index, data);
    CHECK(result.empty());
    REQUIRE(data.inaccuracy);
    CHECK(data.inaccuracy->at(InaccuracyType::Aim) == doctest::Approx(0.15)); // defaults filled

    auto noAmmo = parseGunData(parseJsonText(R"({"fire_mode": ["auto"]})"));
    CHECK(validateGun(index, noAmmo) == "ammo id is empty");

    auto unknownMode = parseGunData(parseJsonText(R"({"ammo": "tacz:a", "fire_mode": ["unknown"]})"));
    CHECK(validateGun(index, unknownMode) == "fire mode is error");

    auto badRecoil = parseGunData(parseJsonText(R"({"ammo": "tacz:a", "fire_mode": ["auto"],
        "recoil": {"pitch": [{"time": 0, "value": [1, 0]}]}})"));
    CHECK(validateGun(index, badRecoil) == "Recoil value's left must be less than right");

    index.type = "  ";
    CHECK(validateGun(index, data) == "index object missing type field");
}
