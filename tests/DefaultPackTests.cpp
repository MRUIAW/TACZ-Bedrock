#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "gun/GunStats.h"
#include "pack/GunPackLoader.h"

using namespace tacz;

#ifdef TACZ_DEFAULT_PACK_DIR

namespace {

struct LoadedDefaultPack {
    pack::GunPackRegistry    registry;
    std::vector<std::string> warnings;

    LoadedDefaultPack() {
        std::vector<std::unique_ptr<pack::PackSource>> packs;
        packs.push_back(pack::openFolderPack(std::filesystem::u8path(TACZ_DEFAULT_PACK_DIR)));
        registry.load(packs, [](std::string const&) {}, [this](std::string const& w) { warnings.push_back(w); });
    }
};

LoadedDefaultPack const& defaultPack() {
    static LoadedDefaultPack pack;
    return pack;
}

pack::ResourceId id(char const* text) { return *pack::ResourceId::parse(text); }

} // namespace

TEST_CASE("Default gun pack loads every gun and ammo without warnings") {
    auto const& pack = defaultPack();
    for (auto const& w : pack.warnings) MESSAGE(w);
    CHECK(pack.warnings.empty());
    CHECK(pack.registry.packs().size() == 1);
    CHECK(pack.registry.packs()[0].meta.ns == "tacz");
    CHECK(pack.registry.guns().size() == 54);
    CHECK(pack.registry.ammos().size() > 0);
    CHECK(pack.registry.dataScripts().contains(id("tacz:xmag_reload_logic")));
    // Every gun's ammo must exist.
    for (auto const& [gunId, gun] : pack.registry.guns()) {
        CAPTURE(gunId.toString());
        CHECK(pack.registry.ammo(*gun.data.ammoId) != nullptr);
    }
}

TEST_CASE("AK47 data matches the pack file") {
    auto const* ak = defaultPack().registry.gun(id("tacz:ak47"));
    REQUIRE(ak);
    CHECK(ak->index.type == "rifle");
    CHECK(ak->index.display == id("tacz:ak47_display"));
    auto const& d = ak->data;
    CHECK(d.ammoId == id("tacz:762x39"));
    CHECK(d.ammoAmount == 30);
    REQUIRE(d.extendedMagAmmoAmount);
    CHECK(*d.extendedMagAmmoAmount == std::vector<int>{34, 37, 40});
    CHECK(d.bolt == pack::Bolt::ClosedBolt);
    CHECK(d.rpm == 600);
    CHECK(d.script == id("tacz:xmag_reload_logic"));
    CHECK(d.scriptParam["reload_feed"].get<double>() == doctest::Approx(1.55));
    CHECK(d.bullet.speed == doctest::Approx(250));
    CHECK(d.bullet.gravity == doctest::Approx(0.15));
    CHECK(d.bullet.life == doctest::Approx(0.8));
    REQUIRE(d.fireModes.size() == 2);
    CHECK(d.fireModes[0] == pack::FireMode::Auto);
    CHECK(d.fireModeAdjust.empty()); // commented out in the pack
    CHECK(d.recoil.pitch.size() == 4);
}

TEST_CASE("AK47 damage, headshot and rate of fire follow TACZ") {
    auto const* ak = defaultPack().registry.gun(id("tacz:ak47"));
    REQUIRE(ak);
    auto dmg = gun::BulletDamage::from(ak->data, pack::FireMode::Auto);

    CHECK(dmg.effectiveRange == doctest::Approx(30));
    CHECK(dmg.atDistance(10) == doctest::Approx(9));
    CHECK(dmg.atDistance(29.9) == doctest::Approx(9));
    CHECK(dmg.atDistance(30) == doctest::Approx(7.5));
    CHECK(dmg.atDistance(59.9) == doctest::Approx(7.5));
    CHECK(dmg.atDistance(500) == doctest::Approx(6));

    CHECK(dmg.headShot == doctest::Approx(1.5));
    auto hit = dmg.onHit(10, true);
    CHECK(hit.normal + hit.armorPiercing == doctest::Approx(13.5));
    CHECK(hit.armorPiercing == doctest::Approx(13.5 * 0.25));

    CHECK(gun::shootIntervalMs(ak->data, pack::FireMode::Auto) == 100);
}

TEST_CASE("Shotgun pellets split damage") {
    for (auto const& [gunId, entry] : defaultPack().registry.guns()) {
        if (entry.data.bullet.bulletAmount <= 1) continue;
        CAPTURE(gunId.toString());
        auto dmg = gun::BulletDamage::from(entry.data, entry.data.fireModes[0].value());
        CHECK(dmg.pelletModifier == doctest::Approx(1.0 / entry.data.bullet.bulletAmount));
    }
}

#endif
