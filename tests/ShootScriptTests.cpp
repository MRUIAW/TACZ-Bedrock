#include <doctest/doctest.h>

#include <limits>
#include <chrono>
#include <iostream>
#include <tuple>
#include "lua/GunLogic.h"
#include "lua/NumericCoercion.h"
#include "pack/GunPackParser.h"

using namespace tacz;
namespace {
struct Source final : pack::PackSource {
    std::string label = "synthetic", script;
    std::string const& name() const override { return label; }
    std::vector<std::string> list(std::string_view prefix) const override {
        return prefix == "data/" ? std::vector<std::string>{"data/fixture/scripts/shoot.lua"} : std::vector<std::string>{};
    }
    std::optional<std::string> read(std::string_view path) const override {
        if (path == "gunpack.meta.json") return R"({"namespace":"fixture"})";
        if (path == "data/fixture/scripts/shoot.lua") return script;
        return {};
    }
};
struct Fixture {
    int64_t now = 1000, last = -1, previous = -1;
    bool alive = true;
    gun::HoldIdentity hold{1, 0, {"fixture", "gun"}};
    gun::GunAmmo ammo{20, true};
    pack::GunData data;
    pack::GunPackRegistry registry;
    std::vector<std::string> warnings;
    std::vector<std::tuple<std::optional<pack::FireMode>, int, bool>> shots;
    std::unique_ptr<lua::GunLogic> logic;
    Fixture(std::string script = "return {}") {
        auto source = std::make_unique<Source>(); source->script = std::move(script);
        std::vector<std::unique_ptr<pack::PackSource>> sources; sources.push_back(std::move(source));
        registry.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
        logic = std::make_unique<lua::GunLogic>(registry, [&](auto const& w) { warnings.push_back(w); }, [&] { return now; });
        data.script = pack::ResourceId{"fixture", "shoot"};
        data.bolt = pack::Bolt::OpenBolt;
        data.burst.count = 3; data.burst.bpm = 600; data.burst.minInterval = .5;
    }
    lua::ScriptApi api(std::optional<pack::FireMode> mode = pack::FireMode::Semi) {
        lua::ScriptApi a; a.data = &data; a.gunId = hold.gunId; a.shooterKey = 1;
        a.ammo = ammo; a.maxAmmo = 20; a.fireMode = mode;
        auto identity = hold;
        auto detached = std::make_shared<gun::GunAmmo>(ammo);
        a.alive = [&] { return alive; };
        a.sameHold = [this, identity] { return hold == identity; };
        a.refresh = [this, identity, detached](lua::ScriptApi& a) {
            if (hold == identity) *detached = ammo;
            a.ammo = *detached;
        };
        a.commit = [this, identity, detached](lua::ScriptApi const& a) {
            *detached = a.ammo;
            if (hold == identity) ammo = a.ammo;
        };
        a.prepareRound = [this, identity](lua::ScriptApi& a, bool consume) {
            auto bolt = a.bolt(); auto mode = a.fireMode; int bullets = a.data->bullet.bulletAmount;
            return [this, identity, bolt, mode, bullets, consume] {
                if (!alive || hold != identity) return false;
                if (consume && !gun::reduceAmmoOnce(bolt, ammo)) return false;
                shots.emplace_back(mode, bullets, consume);
                return true;
            };
        };
        a.wallClock = [&] { return now; };
        a.previousShot = [&] { return previous; };
        a.adjustShot = [&](int64_t alpha) { last = gun::addTime(last, alpha); };
        return a;
    }
    void shoot(std::optional<pack::FireMode> mode = pack::FireMode::Semi) { auto a = api(mode); logic->shoot(a); }
    void tick(int64_t ms) { now = ms; logic->tickTasks(1); }
};
}

TEST_CASE("Cycle scheduler immediate strict period compensation and positive cycle limit") {
    int64_t now = 1000; int calls = 0;
    gun::CycleTasks tasks([&] { return now; });
    REQUIRE(tasks.add([&] { ++calls; return true; }, 0, 100, 4, gun::CycleTasks::Kind::Shot));
    CHECK(calls == 1); CHECK(tasks.pendingShots());
    now = 1100; tasks.tick(); CHECK(calls == 1); // strict >
    now = 1351; tasks.tick(); CHECK(calls == 4); // compensation catch-up
    now = 1401; tasks.tick(); CHECK(calls == 4); CHECK(tasks.size() == 0);
}

TEST_CASE("Cycle scheduler delayed branch retains cumulative elapsed and zero-before-subtract quirks") {
    int64_t now = 1000; int calls = 0;
    gun::CycleTasks tasks([&] { return now; });
    tasks.add([&] { ++calls; return true; }, 300, 100, 10, gun::CycleTasks::Kind::Async);
    CHECK(calls == 0);
    now = 1100; tasks.tick(); CHECK(calls == 0); // delay .3 -> .2, timestamp NOT updated
    now = 1201; tasks.tick(); CHECK(calls == 3); // (.201 + .1), not (.201 - .2 + .1)
    CHECK_FALSE(tasks.pendingShots());
}

TEST_CASE("20 TPS phase contract keeps one pump per tick rather than Java START plus END") {
    int64_t now = 1000, delayedAt = -1, burstAt = -1;
    gun::CycleTasks once([&] { return now; });
    once.add([&] { delayedAt = now; return false; }, 1000, 0, 1, gun::CycleTasks::Kind::Async);
    int rounds = 0;
    once.add([&] { if (++rounds == 2) burstAt = now; return true; }, 0, 100, 2, gun::CycleTasks::Kind::Shot);
    // Production calls tickTasks once after aim/sprint; no END-phase hook.
    // At exactly 100ms strict > keeps the second burst round until 150ms.
    for (now = 1050; now <= 1300; now += 50) once.tick();
    CHECK(delayedAt == 1300); CHECK(burstAt == 1150);

    int64_t javaDelayedAt = -1, javaBurstAt = -1;
    now = 1000; rounds = 0;
    gun::CycleTasks twice([&] { return now; });
    twice.add([&] { javaDelayedAt = now; return false; }, 1000, 0, 1, gun::CycleTasks::Kind::Async);
    twice.add([&] { if (++rounds == 2) javaBurstAt = now; return true; }, 0, 100, 2, gun::CycleTasks::Kind::Shot);
    // Example Java phases: START at 0/50/... and END 10ms after each START.
    for (int64_t start = 1000; start <= 1300; start += 50) {
        now = start; twice.tick(); now = start + 10; twice.tick();
    }
    CHECK(javaDelayedAt == 1210); CHECK(javaBurstAt == 1110);
    CHECK(delayedAt - javaDelayedAt == 90); CHECK(burstAt - javaBurstAt == 40);
}

TEST_CASE("Cycle scheduler unlimited zero negative cycles false stop and nested task staging") {
    for (int cycles : {-1, 0, -2}) {
        int64_t now = 1000; int calls = 0;
        gun::CycleTasks tasks([&] { return now; });
        tasks.add([&] { return ++calls < 5; }, -1, 10, cycles, gun::CycleTasks::Kind::Async);
        now += 100; tasks.tick(); CHECK(calls == 5); CHECK(tasks.size() == 0);
    }
    int64_t now = 1000; int parent = 0, child = 0;
    gun::CycleTasks tasks([&] { return now; });
    tasks.add([&] {
        ++parent;
        tasks.add([&] { ++child; return true; }, 0, 10, 2, gun::CycleTasks::Kind::Shot);
        return false;
    }, 10, 100, 1, gun::CycleTasks::Kind::Async);
    now += 11; tasks.tick(); CHECK(parent == 1); CHECK(child == 1);
    tasks.tick(); CHECK(child == 1);
    now += 11; tasks.tick(); CHECK(child == 2);
}

TEST_CASE("Cycle scheduler task bound cancellation and reentrant lifecycle cleanup") {
    int64_t now = 1000; gun::CycleTasks tasks([&] { return now; });
    for (size_t i = 0; i < gun::CycleTasks::Limit; ++i)
        REQUIRE(tasks.add([] { return true; }, 100, 100, -1, gun::CycleTasks::Kind::Async));
    CHECK_FALSE(tasks.add([] { return true; }, 0, 1, 1, gun::CycleTasks::Kind::Shot));
    tasks.clear(); CHECK(tasks.size() == 0);
    tasks.add([&] { tasks.clear(); return true; }, 0, 1, 1, gun::CycleTasks::Kind::Shot);
    CHECK(tasks.size() == 0);
    tasks.add([] { return true; }, 100, 1, 1, gun::CycleTasks::Kind::Shot);
    tasks.add([] { return true; }, 100, 1, 1, gun::CycleTasks::Kind::Async);
    tasks.cancelShots(); CHECK_FALSE(tasks.pendingShots()); CHECK(tasks.size() == 1);
}

TEST_CASE("Shoot nil defaults missing module and non-function hook errors without fallback") {
    for (auto script : {"return {}", "return {shoot=nil}", "return setmetatable({}, {__index=function() return nil end})"}) {
        Fixture f(script); f.shoot(); CHECK(f.shots.size() == 1); CHECK(f.warnings.empty());
    }
    for (auto script : {"return {shoot=7}", "return setmetatable({}, {__index=function() error('lookup') end})", "return {shoot=function() error('shoot') end}"}) {
        Fixture f(script); f.shoot(); f.shoot(); CHECK(f.shots.empty()); CHECK(f.warnings.size() == 1);
    }
    Fixture f; f.data.script.reset(); f.shoot(); CHECK(f.shots.size() == 1);
}

TEST_CASE("ShootOnce uses LuaJ boolean arguments and each bolt ammo rule") {
    for (auto bolt : {pack::Bolt::OpenBolt, pack::Bolt::ClosedBolt, pack::Bolt::ManualAction}) {
        for (auto argument : {"true", "false", "0", "nil", "'text'", "{}"}) {
            Fixture f(std::string("return {shoot=function(api) api:shootOnce(") + argument + ") end}");
            f.data.bolt = bolt; f.ammo = {2, true}; f.shoot();
            bool consume = std::string(argument) != "false" && std::string(argument) != "nil";
            auto expected = gun::GunAmmo{2, true}; if (consume) gun::reduceAmmoOnce(bolt, expected);
            CHECK(f.ammo == expected); REQUIRE(f.shots.size() == 1); CHECK(std::get<2>(f.shots[0]) == consume);
            CHECK(f.warnings.empty());
        }
        Fixture dry("return {shoot=function(api) api:shootOnce(true); api:shootOnce(false) end}");
        dry.data.bolt = bolt; dry.ammo = {}; dry.shoot(); CHECK(dry.shots.size() == 1);
    }
}

TEST_CASE("Shoot receives only the API and ignores its result while heat receives a timestamp") {
    Fixture f(R"(return {
        shoot=function(...)
            assert(select('#',...) == 1)
            local api = ...
            api:shootOnce(false)
            return false
        end,
        tick_heat=function(...)
            assert(select('#',...) == 2)
            local api, timestamp = ...
            assert(timestamp == 123)
        end
    })");
    f.shoot(); CHECK(f.shots.size() == 1);
    auto a = f.api(); f.logic->tickHeat(a, 123); CHECK(f.warnings.empty());
}

TEST_CASE("Two shootOnce calls are independent concurrent bursts with call-time captures") {
    Fixture f("return {shoot=function(api) api:shootOnce(true); api:shootOnce(true) end}");
    f.data.bullet.bulletAmount = 7;
    f.shoot(pack::FireMode::Burst); CHECK(f.shots.size() == 2);
    f.data.burst.count = 99; f.data.burst.bpm = 1; f.data.bullet.bulletAmount = 99;
    f.tick(1251); CHECK(f.shots.size() == 6); CHECK(f.ammo.magazine == 14);
    for (auto const& [mode, bullets, consume] : f.shots) {
        CHECK(mode == pack::FireMode::Burst); CHECK(bullets == 7); CHECK(consume);
    }
    f.tick(1400); CHECK_FALSE(f.logic->pendingShots(1)); CHECK(f.warnings.empty());
}

TEST_CASE("Absent burst count defaults to three but explicit zero remains unlimited until dry") {
    CHECK(pack::parseGunData(pack::parseJsonText("{}")).burst.count == 3);
    CHECK(pack::parseGunData(pack::parseJsonText(R"({"burst_data":{}})")).burst.count == 3);
    auto parsed = pack::parseGunData(pack::parseJsonText(R"({"burst_data":{"count":0}})"));
    REQUIRE(parsed.burst.count == 0);
    Fixture f; f.data.burst.count = parsed.burst.count; f.ammo = {5, false};
    f.shoot(pack::FireMode::Burst); CHECK(f.shots.size() == 1);
    f.tick(1451); CHECK(f.shots.size() == 5); CHECK(f.ammo.magazine == 0);
    CHECK(f.logic->pendingShots(1));
    f.tick(1551); CHECK(f.shots.size() == 5); CHECK_FALSE(f.logic->pendingShots(1)); CHECK(f.warnings.empty());
}

TEST_CASE("ShootOnce prepares first then immediate cycle rejects a stale hold without mutations") {
    Fixture f("return {shoot=function(api) api:shootOnce(true) end}");
    auto a = f.api(); int prepared = 0;
    auto prepare = a.prepareRound;
    a.prepareRound = [&](lua::ScriptApi& api, bool consume) { ++prepared; return prepare(api, consume); };
    ++f.hold.generation;
    auto before = f.ammo;
    f.logic->shoot(a);
    CHECK(prepared == 1); CHECK(f.shots.empty()); CHECK(f.ammo == before);
    CHECK_FALSE(f.logic->pendingShots(1)); CHECK(f.warnings.empty());
}

TEST_CASE("Burst re-chamber follows the immediate cycle and precedes later cycles") {
    Fixture f(R"(return {shoot=function(api)
        api:shootOnce(true)
        assert(not api:hasAmmoInBarrel())
        assert(api:removeAmmoFromMagazine(1) == 1)
        api:setAmmoInBarrel(true)
    end})");
    f.data.bolt = pack::Bolt::ManualAction; f.ammo = {2, true};
    f.shoot(pack::FireMode::Burst); CHECK(f.shots.size() == 1); CHECK(f.ammo == gun::GunAmmo{1, true});
    f.tick(1101); CHECK(f.shots.size() == 2); CHECK(f.ammo == gun::GunAmmo{1, false});
    f.tick(1201); CHECK(f.shots.size() == 2); CHECK_FALSE(f.logic->pendingShots(1)); CHECK(f.warnings.empty());
}

TEST_CASE("Post-shot mutation and final non-consuming extra shot remain ordered") {
    Fixture f(R"(return {shoot=function(api)
        api:shootOnce(true)
        if api:getAmmoAmount() == 0 then api:shootOnce(false) end
    end})");
    f.ammo = {1, false}; f.shoot(); CHECK(f.shots.size() == 2); CHECK(f.ammo.magazine == 0); CHECK(f.warnings.empty());
}

TEST_CASE("Safe async delayed shot owns its API after the original hook returns") {
    Fixture f(R"(return {shoot=function(api)
        api:safeAsyncTask(function() api:shootOnce(true); return false end, 1000, 0, 1)
    end})");
    f.shoot(); CHECK(f.shots.empty()); CHECK_FALSE(f.logic->pendingShots(1));
    f.tick(1500); CHECK(f.shots.empty());
    f.tick(2000); CHECK(f.shots.size() == 1); CHECK(f.ammo.magazine == 19); CHECK(f.warnings.empty());
}

TEST_CASE("Safe async invalid function non-boolean and callback errors warn once and stop") {
    for (auto body : {"api:safeAsyncTask(1,0,0,1)",
        "api:safeAsyncTask(function() return 1 end,1,0,1)",
        "api:safeAsyncTask(function() error('callback') end,1,0,1)"}) {
        Fixture f(std::string("return {shoot=function(api) ") + body + " end}");
        f.shoot(); f.tick(1002); f.tick(1003); f.shoot(); f.tick(1005);
        CHECK(f.shots.empty()); CHECK(f.warnings.size() == 1);
    }
}

TEST_CASE("Safe async survives switching but shots require the original hold generation") {
    for (int switches : {0, 1, 2}) {
        Fixture f(R"(return {shoot=function(api)
            api:safeAsyncTask(function()
                api:shootOnce(false); api:adjustShootInterval(20)
                api:removeAmmoFromMagazine(2); api:setAmmoInBarrel(false)
                return false
            end,100,0,1)
        end})");
        f.shoot();
        auto original = f.hold;
        if (switches) { ++f.hold.generation; f.hold.slot = 1; f.hold.gunId = {"fixture", "replacement"}; f.ammo = {8, true}; }
        if (switches == 2) { f.hold = original; f.hold.generation += 2; }
        f.tick(1101);
        CHECK(f.shots.size() == (switches ? 0 : 1)); CHECK(f.last == 19);
        CHECK(f.ammo == (switches ? gun::GunAmmo{8, true} : gun::GunAmmo{18, false}));
        CHECK(f.warnings.empty());
    }
}

TEST_CASE("Later shot cycles stop on switching death disconnect and session cleanup") {
    for (int cancellation : {0, 1, 2, 3}) {
        Fixture f; f.shoot(pack::FireMode::Burst); CHECK(f.shots.size() == 1);
        if (cancellation == 0) ++f.hold.generation;
        if (cancellation == 1) f.alive = false;
        if (cancellation >= 2) f.logic->clearTasks(1);
        f.tick(1201); CHECK(f.shots.size() == 1); CHECK_FALSE(f.logic->pendingShots(1));
    }
    Fixture f("return {shoot=function(api) api:safeAsyncTask(function() api:shootOnce(false); return true end,100,100,-1) end}");
    f.shoot(); f.alive = false; f.tick(1201); f.alive = true; f.tick(2000); CHECK(f.shots.empty());
}

TEST_CASE("Heat tick dispatch sees shoot cache and can refill then shoot on a later tick") {
    Fixture f(R"(return {
        shoot=function(api) api:cacheScriptData({pending=true}) end,
        tick_heat=function(api, timestamp)
            assert(timestamp == 12345)
            local cache = api:getCachedScriptData()
            if cache and cache.pending then
                api:putAmmoInMagazine(1)
                api:shootOnce(false)
                cache.pending = false
            end
        end
    })");
    f.ammo = {}; f.shoot(); CHECK(f.shots.empty());
    auto a = f.api(); f.logic->tickHeat(a, 12345); CHECK(f.shots.size() == 1); CHECK(f.ammo.magazine == 1);
    f.logic->tickHeat(a, 12345); CHECK(f.shots.size() == 1); CHECK(f.warnings.empty());
}

TEST_CASE("Heat absent defaults to no-op non-function fails and pre-error mutations persist") {
    Fixture absent; auto a = absent.api(); absent.logic->tickHeat(a, -1); CHECK(absent.shots.empty()); CHECK(absent.warnings.empty());
    for (auto script : {"return {tick_heat=1}", "return {tick_heat=function(api) api:missingHeatMethod() end}"}) {
        Fixture f(script); auto a = f.api(); f.logic->tickHeat(a, -1); f.logic->tickHeat(a, -1); CHECK(f.warnings.size() == 1);
    }
    Fixture f("return {tick_heat=function(api) api:removeAmmoFromMagazine(1); api:missingHeatMethod() end}");
    auto b = f.api(); f.logic->tickHeat(b, -1); CHECK(f.ammo.magazine == 19); CHECK(f.warnings.size() == 1);
}

TEST_CASE("Absent heat hook protected-dispatch benchmark excludes native API construction and NBT clone") {
    Fixture f; auto a = f.api();
    f.logic->tickHeat(a, -1); // load/warm the synthetic module first
    constexpr int repeats = 5000;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < repeats; ++i) f.logic->tickHeat(a, -1);
    auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / repeats;
    std::cout << "[heat benchmark] absent tick_heat protected dispatch: " << us
              << " us/call (native API construction and CompoundTag clone excluded)\n";
    CHECK(f.warnings.empty()); CHECK(f.shots.empty());
}

TEST_CASE("Selection reset drops cache for every shoot reload bolt to heat transition and back") {
    for (auto writer : {"shoot", "start_reload", "start_bolt"}) {
        Fixture f(std::string("return {") + writer + "=function(api) api:cacheScriptData({fromPrevious=true}); return true end,"
            "tick_heat=function(api) assert(api:getCachedScriptData() == nil) end}");
        auto a = f.api();
        if (std::string(writer) == "shoot") f.logic->shoot(a);
        else if (std::string(writer) == "start_reload") CHECK(f.logic->startReload(a) == lua::ReloadStartResult::Started);
        else CHECK(f.logic->startBolt(a));
        ++f.hold.generation; f.logic->clearScriptData(1); auto b = f.api(); b.gunId = {"fixture", "second"};
        f.logic->tickHeat(b, -1);
        ++f.hold.generation; f.logic->clearScriptData(1); f.logic->tickHeat(a, -1);
        CHECK(f.warnings.empty());
    }
}

TEST_CASE("Shoot timing uses previous raw shot plus wall base and interval adjustment delays cooldown") {
    for (auto offset : {std::optional<int64_t>{}, std::optional<int64_t>{400}}) {
        auto base = gun::shotWallBase(offset, 1'000'000, 10'000, 900'000);
        CHECK(base == (offset ? 990'400 : 900'000));
        std::optional<int64_t> current; int64_t previous = 70;
        current.reset(); // initialData resets current, not previous
        previous = current.value_or(-1); current = 100;
        CHECK(gun::addTime(previous, base) == base - 1);
        previous = *current; current = 200;
        CHECK(gun::addTime(previous, base) == base + 100);
    }
    Fixture f(R"(return {shoot=function(api)
        assert(api:getLastShootTimestamp() == 1234)
        assert(api:getCurrentTimestamp() == 1000)
        assert(api:getShootInterval() == 495)
        api:adjustShootInterval('100.9')
    end})");
    f.previous = 1234; f.last = 1000; f.shoot(pack::FireMode::Burst);
    CHECK(f.last == 1100); CHECK(gun::serverCooldownMs(f.data, pack::FireMode::Burst, 1500, f.last) == 95);
    CHECK(f.warnings.empty());
}

TEST_CASE("Reload start strict results keep initialized state on errors and refuse only explicit false") {
    using Result = lua::ReloadStartResult;
    for (auto const& [tail, expected] : std::vector<std::pair<std::string, Result>>{
        {"", Result::Error}, {"return nil", Result::Error},
        {"return 7", Result::Error}, {"return 'true'", Result::Error},
        {"return {}", Result::Error}, {"error('start failed after mutations')", Result::Error},
        {"return false", Result::Refused}, {"return true", Result::Started}}) {
        for (auto initialState : {gun::ReloadStateType::EmptyFeeding, gun::ReloadStateType::TacticalFeeding}) {
            CAPTURE(tail);
            CAPTURE(initialState);
            Fixture f(std::string(R"(return {
                start_reload=function(api)
                    api:cacheScriptData({pending=true})
                    api:putAmmoInMagazine(1)
                    api:setAmmoInBarrel(true)
                    api:adjustBoltTime(25)
            )") + tail + R"(
                end,
                tick_reload=function(api)
                    assert(api:getCachedScriptData().pending)
                    api:putAmmoInMagazine(4)
                    return NOT_RELOADING, -1
                end,
                start_bolt=function(api)
                    assert(api:getCachedScriptData().pending)
                    return false
                end
            })");
            f.ammo = {0, false};
            auto a = f.api(); a.reloadState = initialState; a.boltTimestamp = 500;
            int64_t committedBolt = 500;
            auto commitAmmo = a.commit;
            a.commit = [&, commitAmmo](lua::ScriptApi const& api) { commitAmmo(api); committedBolt = api.boltTimestamp; };
            auto result = f.logic->startReload(a);
            CHECK(result == expected);
            CHECK(a.reloadState == initialState);
            CHECK(a.ammo == gun::GunAmmo{1, true}); CHECK(f.ammo == a.ammo);
            CHECK(a.boltTimestamp == 475); CHECK(committedBolt == 475);
            CHECK_FALSE(f.logic->startBolt(a)); // proves the cache survived every outcome
            CHECK(f.warnings.size() == (expected == Result::Error ? 1 : 0));
            if (expected == Result::Error) {
                CHECK(f.warnings.front().find("start_reload failed for fixture:gun") != std::string::npos);
                if (tail.find("error(") == std::string::npos)
                    CHECK(f.warnings.front().find("boolean result expected") != std::string::npos);
            }

            // LivingEntityReload/handleReload state is initialized before dispatch;
            // only Refused resets it. Error must remain eligible for tick_reload.
            auto state = initialState; int64_t startedAt = 1000;
            if (result == Result::Refused) { state = gun::ReloadStateType::NotReloading; startedAt = -1; }
            CHECK(gun::isReloading(state) == (expected != Result::Refused));
            CHECK(startedAt == (expected == Result::Refused ? -1 : 1000));
            if (gun::isReloading(state)) {
                auto next = f.logic->tickReload(a);
                CHECK(next.type == gun::ReloadStateType::NotReloading);
                CHECK(f.ammo == gun::GunAmmo{5, true});
            } else CHECK(f.ammo == gun::GunAmmo{1, true});
            CHECK(f.logic->startReload(a) == expected);
            CHECK(f.warnings.size() == (expected == Result::Error ? 1 : 0)); // warn once, not once per attempt
        }
    }
}

TEST_CASE("Reload start absent hook defaults true but present non-function or lookup error keeps started with warning") {
    using Result = lua::ReloadStartResult;
    for (auto const& [hook, expected] : std::vector<std::pair<std::string, Result>>{
        {"", Result::Started}, {"start_reload=nil,", Result::Started},
        {"start_reload=false,", Result::Error}, {"start_reload=1,", Result::Error},
        {"start_reload='text',", Result::Error}, {"start_reload={},", Result::Error}}) {
        CAPTURE(hook);
        Fixture f("return {" + hook + R"(tick_reload=function(api)
            api:putAmmoInMagazine(3); return NOT_RELOADING,-1 end})");
        f.ammo = {}; auto a = f.api(); a.reloadState = gun::ReloadStateType::EmptyFeeding;
        CHECK(f.logic->startReload(a) == expected);
        CHECK(f.logic->startReload(a) == expected);
        CHECK(f.warnings.size() == (expected == Result::Error ? 1 : 0));
        if (expected == Result::Error) CHECK(f.warnings.front().find("function or nil expected") != std::string::npos);
        CHECK(f.logic->tickReload(a).type == gun::ReloadStateType::NotReloading);
        CHECK(f.ammo.magazine == 3);
    }
    Fixture lookup(R"(return setmetatable({}, {__index=function() error('lookup failed') end}))");
    auto a = lookup.api();
    CHECK(lookup.logic->startReload(a) == Result::Error);
    CHECK(lookup.logic->startReload(a) == Result::Error);
    REQUIRE(lookup.warnings.size() == 1);
    CHECK(lookup.warnings[0].find("lookup failed") != std::string::npos);
    Fixture noScript; noScript.data.script.reset(); a = noScript.api();
    CHECK(noScript.logic->startReload(a) == Result::Started); CHECK(noScript.warnings.empty());
}

TEST_CASE("Reload tick runtime error still returns idle and preserves pre-error mutations without retry") {
    Fixture f(R"(return {
        start_reload=function(api) api:cacheScriptData({ticks=0}); return true end,
        tick_reload=function(api)
            local cache = api:getCachedScriptData(); cache.ticks = cache.ticks + 1
            api:putAmmoInMagazine(1)
            error('tick failed')
        end,
        start_bolt=function(api) assert(api:getCachedScriptData().ticks == 1); return false end
    })");
    f.ammo = {}; auto a = f.api(); a.reloadState = gun::ReloadStateType::EmptyFeeding;
    REQUIRE(f.logic->startReload(a) == lua::ReloadStartResult::Started);
    for (int tick = 0; tick < 3; ++tick) if (gun::isReloading(a.reloadState)) a.reloadState = f.logic->tickReload(a).type;
    CHECK(a.reloadState == gun::ReloadStateType::NotReloading); CHECK(f.ammo.magazine == 1);
    CHECK_FALSE(f.logic->startBolt(a)); // server stops calling tick_reload after the first idle result
    REQUIRE(f.warnings.size() == 1); CHECK(f.warnings[0].find("tick_reload failed") != std::string::npos);
}

TEST_CASE("Figura LuaJ numeric coercion saturates long narrows int and defaults non-numbers") {
    sol::state state;
    auto number = [&](double d) { return sol::make_object(state, d); };
    auto text = [&](std::string s) { return sol::make_object(state, s); };
    CHECK(lua::luaJLong(number(12.9)) == 12); CHECK(lua::luaJLong(number(-12.9)) == -12);
    CHECK(lua::luaJLong(number(std::numeric_limits<double>::quiet_NaN())) == 0);
    CHECK(lua::luaJLong(number(std::numeric_limits<double>::infinity())) == INT64_MAX);
    CHECK(lua::luaJLong(number(-std::numeric_limits<double>::infinity())) == INT64_MIN);
    CHECK(lua::luaJInt(number(4294967297.0)) == 1); CHECK(lua::luaJInt(number(2147483648.0)) == INT32_MIN);
    CHECK(lua::luaJInt(number(0x1p63)) == -1);
    for (auto s : {"", "word", "\t12", "0x1p2", "nan", "inf"}) CHECK(lua::luaJLong(text(s)) == 0);
    CHECK(lua::luaJLong(text(" 12.9 ")) == 12); CHECK(lua::luaJLong(text("0x10")) == 16);
    CHECK(lua::luaJLong(sol::make_object(state, true)) == 0);
    CHECK(lua::luaJLong(sol::make_object(state, state.create_table())) == 0);
    CHECK(lua::luaJLong(sol::make_object(state, sol::lua_nil)) == 0);
    Fixture f(R"(return {shoot=function(api)
        api:safeAsyncTask(function() api:shootOnce(false); return false end,'100.9',{},4294967297)
    end})");
    f.shoot(); f.tick(1101); CHECK(f.shots.size() == 1); CHECK(f.warnings.empty());
}
