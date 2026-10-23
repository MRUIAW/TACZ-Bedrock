#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "lua/GunLogic.h"
#include "pack/AnimationSounds.h"
#include "pack/GunPackLoader.h"
#include "pack/GunPackParser.h"

using namespace tacz;
using gun::ReloadStateType;

namespace {

// Runs a reload to completion at a fixed tick, like LivingEntityReload.tickReloadState on
// the server (50 ms ticks). Returns the state sequence and the final ammo.
struct ReloadRun {
    std::vector<ReloadStateType> states;
    gun::GunAmmo                 ammo;
    int64_t                      finishedAt = -1;
};

ReloadRun runReload(lua::GunLogic& logic, lua::ScriptApi api, bool empty) {
    ReloadRun run;
    api.reloadState  = empty ? ReloadStateType::EmptyFeeding : ReloadStateType::TacticalFeeding;
    api.reloadTimeMs = 0;
    REQUIRE(logic.startReload(api));
    for (int64_t t = 0; t <= 10'000; t += 50) {
        api.reloadTimeMs = t;
        auto state       = logic.tickReload(api);
        if (run.states.empty() || run.states.back() != state.type) run.states.push_back(state.type);
        api.reloadState = state.type;
        if (state.type == ReloadStateType::NotReloading) {
            run.finishedAt = t;
            break;
        }
    }
    run.ammo = api.ammo;
    return run;
}

struct DefaultPack {
    pack::GunPackRegistry registry;
    DefaultPack() {
        std::vector<std::unique_ptr<pack::PackSource>> packs;
        packs.push_back(pack::openFolderPack(std::filesystem::u8path(TACZ_DEFAULT_PACK_DIR)));
        registry.load(std::move(packs), [](std::string const&) {}, [](std::string const&) {});
    }
};

DefaultPack& defaultPack() {
    static DefaultPack pack;
    return pack;
}

lua::ScriptApi apiFor(pack::GunEntry const& gun, gun::GunAmmo ammo) {
    lua::ScriptApi api;
    api.gunId         = gun.id;
    api.data          = &gun.data;
    api.ammo          = ammo;
    api.maxAmmo       = gun.data.ammoAmount;
    api.needCheckAmmo = false; // creative: magazine is simply filled
    return api;
}

} // namespace

TEST_CASE("Default reload (no script): feed then finish, ammo on the feed edge") {
    pack::GunPackRegistry empty;
    std::vector<std::string> warnings;
    lua::GunLogic logic(empty, [&](std::string const& w) { warnings.push_back(w); });

    auto data = pack::parseGunData(pack::parseJsonText(R"({"ammo": "tacz:a", "ammo_amount": 30, "bolt": "closed_bolt",
        "reload": {"feed": {"empty": 2.0, "tactical": 1.5}, "cooldown": {"empty": 2.5, "tactical": 2.0}}})"));
    pack::GunEntry gun{*pack::ResourceId::parse("tacz:test"), {}, data, "test"};

    auto tactical = runReload(logic, apiFor(gun, {10, true}), false);
    CHECK(tactical.states == std::vector{ReloadStateType::TacticalFeeding, ReloadStateType::TacticalFinishing, ReloadStateType::NotReloading});
    CHECK(tactical.ammo == gun::GunAmmo{30, true});
    CHECK(tactical.finishedAt == 2000);

    // Empty reload chambers a round from the fresh magazine.
    auto emptyRun = runReload(logic, apiFor(gun, {0, false}), true);
    CHECK(emptyRun.ammo == gun::GunAmmo{29, true});
    CHECK(emptyRun.finishedAt == 2500);
    CHECK(warnings.empty());
}

TEST_CASE("Survival reload takes only what the inventory has") {
    pack::GunPackRegistry empty;
    lua::GunLogic logic(empty, [](std::string const&) {});
    auto data = pack::parseGunData(pack::parseJsonText(R"({"ammo": "tacz:a", "ammo_amount": 30, "bolt": "open_bolt"})"));
    pack::GunEntry gun{*pack::ResourceId::parse("tacz:test"), {}, data, "test"};

    auto api          = apiFor(gun, {5, false});
    api.needCheckAmmo = true;
    int stock         = 12;
    api.extractAmmo   = [&](int n) {
        int taken = std::min(n, stock);
        stock -= taken;
        return taken;
    };
    auto run = runReload(logic, api, false);
    CHECK(run.ammo.magazine == 17);
    CHECK(stock == 0);
}

#ifdef TACZ_DEFAULT_PACK_DIR

TEST_CASE("AK47 reloads through xmag_reload_logic.lua with its script_param timings") {
    std::vector<std::string> warnings;
    lua::GunLogic logic(defaultPack().registry, [&](std::string const& w) { warnings.push_back(w); });
    auto const* ak = defaultPack().registry.gun(*pack::ResourceId::parse("tacz:ak47"));
    REQUIRE(ak);
    REQUIRE(ak->data.script);
    CHECK(logic.loadScript(*ak->data.script));

    // reload_feed 1.55 / reload_cooldown 2.0; empty_feed 2.25 / empty_cooldown 2.6.
    auto tactical = runReload(logic, apiFor(*ak, {12, true}), false);
    CHECK(tactical.states == std::vector{ReloadStateType::TacticalFeeding, ReloadStateType::TacticalFinishing, ReloadStateType::NotReloading});
    CHECK(tactical.ammo == gun::GunAmmo{30, true});
    CHECK(tactical.finishedAt == 2000);

    auto emptyRun = runReload(logic, apiFor(*ak, {0, false}), true);
    CHECK(emptyRun.ammo == gun::GunAmmo{29, true});
    CHECK(emptyRun.finishedAt == 2600);

    for (auto const& w : warnings) MESSAGE(w);
    CHECK(warnings.empty());
}

TEST_CASE("AK47 reload animations carry their sound keyframes") {
    auto const* display = defaultPack().registry.gunDisplay(*pack::ResourceId::parse("tacz:ak47_display"));
    REQUIRE(display);
    REQUIRE(display->animation);
    auto text = defaultPack().registry.readFile(pack::assetPath(*display->animation, "animations", ".animation.json"));
    REQUIRE(text);
    auto sounds = pack::parseAnimationSounds(pack::parseJsonText(*text));
    REQUIRE(sounds.contains("reload_tactical"));
    auto const& tactical = sounds.at("reload_tactical");
    REQUIRE(tactical.size() == 6);
    CHECK(tactical[1].time == doctest::Approx(0.35));
    CHECK(tactical[1].sound.toString() == "tacz:ak47/ak47_reload_magout");
    CHECK(sounds.at("reload_empty").size() == 9);
    // Every keyframe sound with a namespace exists in the pack.
    for (auto const& [name, list] : sounds) {
        for (auto const& s : list) {
            if (s.sound.ns != "tacz") continue; // "ar_akilo_drop" etc. have no namespace in the pack
            CAPTURE(s.sound.toString());
            CHECK(defaultPack().registry.readFile(pack::assetPath(s.sound, "tacz_sounds", ".ogg")).has_value());
        }
    }
}

TEST_CASE("Every default-pack gun logic script loads in the sandbox") {
    std::vector<std::string> warnings;
    lua::GunLogic logic(defaultPack().registry, [&](std::string const& w) { warnings.push_back(w); });
    for (auto const& [id, source] : defaultPack().registry.dataScripts()) {
        CAPTURE(id.toString());
        CHECK(logic.loadScript(id));
    }
    for (auto const& w : warnings) MESSAGE(w);
    CHECK(warnings.empty());
}

#endif

TEST_CASE("Sandbox: no file access, no bytecode, runaway scripts are stopped") {
    struct ScriptPack : pack::PackSource {
        std::string mName = "inline";
        std::string const& name() const override { return mName; }
        std::vector<std::string> list(std::string_view prefix) const override {
            if (prefix != "data/") return {};
            return {"data/t/scripts/io.lua", "data/t/scripts/spin.lua"};
        }
        std::optional<std::string> read(std::string_view path) const override {
            if (path == "gunpack.meta.json") return R"({"namespace": "t"})";
            if (path == "data/t/scripts/io.lua") {
                return "if io or os or dofile or loadfile or string.dump or package.loadlib then error('unsafe') end\n"
                       "assert(not load('\\27Lua'), 'binary chunk accepted')\n"
                       "return {}";
            }
            if (path == "data/t/scripts/spin.lua") return "local M = {} function M.start_reload(api) while true do end end return M";
            return std::nullopt;
        }
    };
    std::vector<std::unique_ptr<pack::PackSource>> packs;
    packs.push_back(std::make_unique<ScriptPack>());
    pack::GunPackRegistry registry;
    registry.load(std::move(packs), [](std::string const&) {}, [](std::string const&) {});

    std::vector<std::string> warnings;
    lua::GunLogic logic(registry, [&](std::string const& w) { warnings.push_back(w); });
    CHECK(logic.loadScript(*pack::ResourceId::parse("t:io")));
    for (auto const& w : warnings) MESSAGE(w);
    REQUIRE(warnings.empty());

    auto data   = pack::parseGunData(pack::parseJsonText(R"({"ammo": "tacz:a", "script": "t:spin"})"));
    pack::GunEntry gun{*pack::ResourceId::parse("t:spin_gun"), {}, data, "inline"};
    auto api    = apiFor(gun, {0, false});
    CHECK_FALSE(logic.startReload(api)); // aborted by the instruction limit -> reload refused
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("instructions") != std::string::npos);
}
