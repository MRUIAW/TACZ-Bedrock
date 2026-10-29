#include <doctest/doctest.h>

#include <future>
#include <tuple>
#include "lua/GunLogic.h"
#include "gun/ClientStateLock.h"
#include "gun/ClientTickGate.h"

using namespace tacz;
namespace {
lua::ScriptApi apiFor(pack::GunData const& data, gun::GunAmmo ammo = {3, false}) {
    lua::ScriptApi api; api.data = &data; api.ammo = ammo; api.maxAmmo = 5; api.isBolting = true; return api;
}
pack::GunData dataFor(float action, float feed = -1) {
    pack::GunData data; data.bolt = pack::Bolt::ManualAction; data.boltActionTime = action; data.boltFeedTime = feed; return data;
}
class ScriptSource final : public pack::PackSource {
    std::string label = "synthetic";
public:
    std::map<std::string, std::string> files;
    ScriptSource() { files["gunpack.meta.json"] = R"({"namespace":"fixture"})"; }
    std::string const& name() const override { return label; }
    std::vector<std::string> list(std::string_view prefix) const override {
        std::vector<std::string> result;
        for (auto const& [path, _] : files) if (path.starts_with(prefix)) result.push_back(path);
        return result;
    }
    std::optional<std::string> read(std::string_view path) const override {
        auto found = files.find(std::string(path));
        return found == files.end() ? std::nullopt : std::optional(found->second);
    }
};

struct BoltScripts {
    pack::GunPackRegistry registry;
    std::vector<std::string> warnings;
    std::unique_ptr<lua::GunLogic> logic;
    pack::GunData data = dataFor(.6f, .2f);
    explicit BoltScripts(std::string const& script) {
        auto source = std::make_unique<ScriptSource>();
        source->files["data/fixture/scripts/bolt.lua"] = script;
        std::vector<std::unique_ptr<pack::PackSource>> sources; sources.push_back(std::move(source));
        registry.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
        logic = std::make_unique<lua::GunLogic>(registry, [&](auto const& w) { warnings.push_back(w); });
        data.script = pack::ResourceId{"fixture", "bolt"};
    }
    lua::ScriptApi api() {
        auto result = apiFor(data); result.gunId = {"fixture", "gun"}; result.isBolting = false; return result;
    }
};
}

TEST_CASE("Default bolt feeds once at feed time and remains active until action time") {
    auto data = dataFor(.9f, .3f); auto api = apiFor(data);
    api.boltNowMs = 299; CHECK(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{3, false});
    api.boltNowMs = 300; CHECK(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    api.boltNowMs = 899; CHECK(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    api.boltNowMs = 900; CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    api.boltNowMs = 950; CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
}

TEST_CASE("Default bolt exact minus one feed means action while other negative feed times do not") {
    auto data = dataFor(.57f); auto api = apiFor(data);
    CHECK(gun::boltMillis(.57f) == 570);
    api.boltNowMs = 569; CHECK(lua::defaultTickBolt(api)); CHECK_FALSE(api.ammo.barrel);
    api.boltNowMs = 570; CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    for (float feed : {-.5f, -1.0001f}) {
        data.boltFeedTime = feed; api = apiFor(data);
        CHECK(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    }
}

TEST_CASE("Default bolt supports zero action no clip and feed later than action") {
    auto zero = dataFor(0); auto api = apiFor(zero);
    CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    auto late = dataFor(.1f, .4f); api = apiFor(late);
    for (int64_t time : {0, 100, 399}) { api.boltNowMs = time; CHECK(lua::defaultTickBolt(api)); CHECK_FALSE(api.ammo.barrel); }
    api.boltNowMs = 400; CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
}

TEST_CASE("Default bolt does not invent feed ammo or consume magazine ammo for an already chambered item") {
    auto data = dataFor(0);
    auto api = apiFor(data, {0, false}); CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{0, false});
    api = apiFor(data, {3, true}); CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{3, true});
    data.boltActionTime = .5f; data.boltFeedTime = .1f;
    api = apiFor(data, {0, false}); api.boltNowMs = 100;
    CHECK(lua::defaultTickBolt(api)); CHECK_FALSE(api.ammo.barrel);
    api.boltNowMs = 500; CHECK_FALSE(lua::defaultTickBolt(api)); CHECK_FALSE(api.ammo.barrel);
}

TEST_CASE("Default inventory bolt consumes exactly one stock round or creative virtual round") {
    auto data = dataFor(.5f, .1f); data.reload.type = pack::FeedType::Inventory;
    for (bool creative : {false, true}) for (int stock : {0, 2}) {
        auto api = apiFor(data, {0, false}); api.needCheckAmmo = !creative;
        int original = stock, calls = 0;
        api.extractAmmo = [&](int n) { ++calls; int take = std::min(n, stock); stock -= take; return take; };
        api.boltNowMs = 99; CHECK(lua::defaultTickBolt(api)); CHECK(calls == 0);
        api.boltNowMs = 100; CHECK(lua::defaultTickBolt(api));
        CHECK(api.ammo.barrel == (creative || original > 0)); CHECK(api.ammo.magazine == 0);
        CHECK(stock == (creative || !original ? original : original - 1));
        CHECK(calls == (creative ? 0 : 1));
        if (api.ammo.barrel) { CHECK(lua::defaultTickBolt(api)); CHECK(calls == (creative ? 0 : 1)); }
    }
}

TEST_CASE("Bolt elapsed API is zero when inactive and otherwise preserves elapsed wall-clock milliseconds") {
    lua::ScriptApi api; api.boltNowMs = 123;
    CHECK(api.getBoltTime() == 0);
    api.isBolting = true; CHECK(api.getBoltTime() == 123);
    api.boltNowMs = -12; CHECK(api.getBoltTime() == -12);
    api.isBolting = false; CHECK(api.getBoltTime() == 0);
    CHECK(gun::boltMillis(std::numeric_limits<float>::infinity()) == std::numeric_limits<int64_t>::max());
    CHECK(gun::boltMillis(-std::numeric_limits<float>::infinity()) == std::numeric_limits<int64_t>::min());
    CHECK(gun::boltMillis(std::numeric_limits<float>::quiet_NaN()) == 0);
}

TEST_CASE("Server bolt checks nonzero cooldown reload draw and bolting while client checks lock and local latch") {
    for (int64_t shoot : {-1, 0, 1}) for (int64_t draw : {-1, 0, 1})
    for (bool reload : {false, true}) for (bool bolting : {false, true})
    for (bool locked : {false, true}) for (bool latched : {false, true}) {
        gun::BoltCheck c; c.ammo = {3, false}; c.shootCooldown = shoot; c.drawCooldown = draw;
        c.reload = reload ? gun::ReloadStateType::TacticalFinishing : gun::ReloadStateType::NotReloading;
        c.serverBolting = bolting; c.clientLocked = locked; c.localBolting = latched;
        CHECK((gun::serverBoltRefusal(c) == nullptr) == (shoot == 0 && draw == 0 && !reload && !bolting));
        CHECK((gun::clientBoltRefusal(c) == nullptr) == (!locked && !latched));
    }
}

TEST_CASE("Bolt ammo precondition matrix includes inventory stock creative chamber and all bolt types") {
    for (auto type : {pack::Bolt::ManualAction, pack::Bolt::OpenBolt, pack::Bolt::ClosedBolt})
    for (int magazine : {0, 1}) for (bool chamber : {false, true}) for (bool inventory : {false, true})
    for (bool stock : {false, true}) for (bool creative : {false, true}) {
        gun::BoltCheck c; c.type = type; c.ammo = {magazine, chamber}; c.inventoryFed = inventory;
        c.inventoryHasAmmo = stock; c.needCheckAmmo = !creative;
        bool expected = type == pack::Bolt::ManualAction && !chamber && (inventory ? creative || stock : magazine > 0);
        CHECK((gun::serverBoltRefusal(c) == nullptr) == expected);
        CHECK((gun::clientBoltRefusal(c) == nullptr) == expected);
    }
}

TEST_CASE("Bolt refuses missing gun data and display without starting an operation") {
    gun::BoltCheck c; c.ammo = {1, false}; CHECK_FALSE(gun::serverBoltRefusal(c)); CHECK_FALSE(gun::clientBoltRefusal(c));
    c.hasGun = false; CHECK(gun::serverBoltRefusal(c)); CHECK(gun::clientBoltRefusal(c)); c.hasGun = true;
    c.knownData = false; CHECK(gun::serverBoltRefusal(c)); CHECK(gun::clientBoltRefusal(c)); c.knownData = true;
    c.hasDisplay = false; CHECK_FALSE(gun::serverBoltRefusal(c)); CHECK(gun::clientBoltRefusal(c)); c.hasDisplay = true;
}

TEST_CASE("Local bolt latch survives refusal and lock timeout and clears only by item draw or respawn reset") {
    gun::ClientBoltLatch latch; latch.isBolting = true;
    gun::ClientStateLock lock; lock.lockState(gun::LockCondition::Bolt, 0);
    gun::sync::Snapshot idle; idle.set({gun::sync::Key::ShootCoolDown, int64_t{0}});
    idle.set({gun::sync::Key::DrawCoolDown, int64_t{0}}); idle.set({gun::sync::Key::MeleeCoolDown, int64_t{0}});
    CHECK_FALSE(lock.tickStateLock(249, idle)); CHECK(lock.clientStateLock);
    CHECK(lock.tickStateLock(250, idle)); CHECK_FALSE(lock.clientStateLock); CHECK(latch.isBolting);
    latch.observeItem(true, false); CHECK(latch.isBolting);
    latch.observeItem(true, true); CHECK_FALSE(latch.isBolting);
    latch.isBolting = true; latch.observeItem(false, false); CHECK_FALSE(latch.isBolting);
    latch.isBolting = true; latch.reset(); CHECK_FALSE(latch.isBolting); // draw or world/respawn
}

TEST_CASE("Early fed bolt still refuses server shoot reload and client shooting until authoritative completion") {
    auto data = dataFor(.8f, .2f); auto api = apiFor(data); api.boltNowMs = 200;
    bool active = lua::defaultTickBolt(api); CHECK(active); CHECK(api.ammo.barrel);
    CHECK(gun::boltBlocksAction(active)); // same predicate wired to both server action handlers
    gun::sync::Snapshot state;
    state.set({gun::sync::Key::DrawCoolDown, int64_t{0}}); state.set({gun::sync::Key::MeleeCoolDown, int64_t{0}});
    state.set({gun::sync::Key::IsBolting, active}); CHECK_FALSE(gun::syncedShootPrecheck(state));
    gun::ClientStateLock lock; lock.lockState(gun::LockCondition::Bolt, 0);
    CHECK_FALSE(lock.tickStateLock(10, state)); CHECK(lock.clientStateLock);
    CHECK(lock.lockedCondition == gun::LockCondition::None);
    api.boltNowMs = 800; active = lua::defaultTickBolt(api); CHECK_FALSE(gun::boltBlocksAction(active));
    state.set({gun::sync::Key::IsBolting, active}); CHECK(gun::syncedShootPrecheck(state));
    CHECK_FALSE(lock.tickStateLock(800, state)); CHECK_FALSE(lock.clientStateLock);
}

TEST_CASE("Completed tick auto bolt gate skips repeated frames without consuming trigger state") {
    gun::ClientTickGate gate;
    int bolts = 0;
    for (uint64_t completed : {0, 0, 1, 1, 1, 2, 2}) if (completed && gate.tryAcquire(completed)) ++bolts;
    CHECK(bolts == 2);
    gate.reset(); CHECK(gate.tryAcquire(1));
}

TEST_CASE("Frame observes a chambered bolt latch before a same-frame shot and the next tick bolts again") {
    gun::ClientBoltLatch latch; latch.isBolting = true;
    gun::ClientTickGate gate;
    gun::GunAmmo ammo{3, false};
    int attempts = 0, started = 0;
    auto frame = [&](uint64_t completedTick, bool fire) {
        // Same ordering as frame(): observation is outside the completed-tick gate.
        latch.observeItem(true, ammo.barrel);
        if (completedTick && gate.tryAcquire(completedTick)) {
            ++attempts;
            gun::BoltCheck check; check.ammo = ammo; check.localBolting = latch.isBolting;
            if (!gun::clientBoltRefusal(check)) { latch.isBolting = true; ++started; }
            latch.observeItem(true, ammo.barrel);
        }
        if (fire) {
            CHECK_FALSE(latch.isBolting); // observed before this shot consumes the chamber
            REQUIRE(gun::prepareShot(pack::Bolt::ManualAction, ammo) == gun::ShotCheck::Ok);
            REQUIRE(gun::reduceAmmoOnce(pack::Bolt::ManualAction, ammo));
        }
    };
    frame(1, false); // previous bolt still latched; this tick's auto-bolt gate is spent
    CHECK(latch.isBolting); CHECK(attempts == 1); CHECK(started == 0);
    ammo.barrel = true; // chambered item sync between completed ticks
    frame(1, true); // no new tick, but observation must precede the shot
    CHECK_FALSE(latch.isBolting); CHECK_FALSE(ammo.barrel);
    CHECK(ammo.magazine == 3); CHECK(attempts == 1); CHECK(started == 0);
    frame(2, false); // the next tick can bolt the now-empty chamber again
    CHECK(latch.isBolting); CHECK(attempts == 2); CHECK(started == 1);
    frame(2, false);
    CHECK(latch.isBolting); CHECK(attempts == 2); CHECK(started == 1);
}

TEST_CASE("Lua bolt dispatch uses executed module tables and inherited or dynamic hooks") {
    auto source = std::make_unique<ScriptSource>();
    source->files["data/fixture/scripts/base.lua"] = "return {start_bolt=function(api) api:putAmmoInMagazine(1); return false end}";
    source->files["data/fixture/scripts/inherited.lua"] = "return setmetatable({}, {__index=require('fixture_base')})";
    source->files["data/fixture/scripts/dynamic.lua"] = "local t={}; t['start_'..'bolt']=function(api) api:putAmmoInMagazine(1); return false end; return t";
    source->files["data/fixture/scripts/reload.lua"] = "-- start_bolt is only a comment\nreturn {start_reload=function() return true end}";
    source->files["data/fixture/scripts/lookup.lua"] = "return setmetatable({}, {__index=function(_, key) local n=0; for i=1,100000 do n=n+i end; if key=='tick_bolt' then return function() return true end end end})";
    std::vector<std::unique_ptr<pack::PackSource>> sources; sources.push_back(std::move(source));
    pack::GunPackRegistry registry; registry.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    std::vector<std::string> warnings; lua::GunLogic logic(registry, [&](auto const& w) { warnings.push_back(w); });
    auto data = dataFor(.5f);
    auto api = apiFor(data); CHECK(logic.startBolt(api));
    for (auto name : {"base", "inherited", "dynamic"}) {
        data.script = pack::ResourceId{"fixture", name}; api = apiFor(data);
        CHECK_FALSE(logic.startBolt(api)); CHECK(api.ammo.magazine == 4);
    }
    data.script = pack::ResourceId{"fixture", "reload"}; CHECK(logic.startBolt(api));
    data.script = pack::ResourceId{"fixture", "lookup"};
    for (int attempt = 0; attempt < 30; ++attempt) CHECK(logic.tickBolt(api));
    CHECK(warnings.empty());
}

TEST_CASE("Independent gun logic VMs keep independent instruction budgets") {
    auto source = std::make_unique<ScriptSource>();
    source->files["data/fixture/scripts/work.lua"] = "local n=0; for i=1,500000 do n=n+i end; return {}";
    std::vector<std::unique_ptr<pack::PackSource>> sources; sources.push_back(std::move(source));
    pack::GunPackRegistry registry; registry.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    auto run = [&] { lua::GunLogic logic(registry, [](auto const&) {}); return logic.loadScript({"fixture", "work"}); };
    auto other = std::async(std::launch::async, run);
    CHECK(run()); CHECK(other.get());
}

TEST_CASE("Lua bolt hooks read script params feed once and finish independently of default times") {
    BoltScripts fixture(R"(
        return {
            start_bolt=function(api)
                assert(api:getBoltTime() == 0)
                api:cacheScriptData({started=true})
                return true
            end,
            tick_bolt=function(api)
                assert(api:getCachedScriptData().started)
                local params = api:getScriptParams()
                local time = api:getBoltTime()
                if time < params.feed * 1000 then return true end
                if not api:hasAmmoInBarrel() and api:removeAmmoFromMagazine(1) ~= 0 then
                    api:setAmmoInBarrel(true)
                end
                return time < params.total * 1000
            end
        }
    )");
    fixture.data.scriptParam = {{"feed", .3}, {"total", .9}};
    auto api = fixture.api(); api.boltTimestamp = 1000; api.boltNowMs = 1100;
    CHECK(api.getBoltTime() == 0);
    api.isBolting = fixture.logic->startBolt(api); REQUIRE(api.isBolting);
    CHECK(api.getBoltTime() == 100);
    for (auto [now, active, ammo] : {
        std::tuple{1299, true, gun::GunAmmo{3, false}}, {1300, true, gun::GunAmmo{2, true}},
        {1899, true, gun::GunAmmo{2, true}}, {1900, false, gun::GunAmmo{2, true}}}) {
        api.boltNowMs = now; api.isBolting = fixture.logic->tickBolt(api);
        CHECK(api.isBolting == active); CHECK(api.ammo == ammo);
    }
    CHECK(api.getBoltTime() == 0); CHECK(fixture.warnings.empty());
}

TEST_CASE("Lua bolt defaults each absent hook independently") {
    for (auto script : {"return {}", "return {start_bolt=function() return true end}",
                        "return {start_bolt=nil, tick_bolt=nil}"}) {
        BoltScripts fixture(script); auto api = fixture.api();
        api.isBolting = fixture.logic->startBolt(api); REQUIRE(api.isBolting);
        api.boltNowMs = 199; CHECK(fixture.logic->tickBolt(api)); CHECK_FALSE(api.ammo.barrel);
        api.boltNowMs = 200; CHECK(fixture.logic->tickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
        api.boltNowMs = 600; CHECK_FALSE(fixture.logic->tickBolt(api)); CHECK(fixture.warnings.empty());
    }
    BoltScripts tickOnly("return {tick_bolt=function(api) api:putAmmoInMagazine(1); return false end}");
    auto api = tickOnly.api(); api.isBolting = tickOnly.logic->startBolt(api); REQUIRE(api.isBolting);
    CHECK_FALSE(tickOnly.logic->tickBolt(api)); CHECK(api.ammo == gun::GunAmmo{4, false});
    CHECK(tickOnly.warnings.empty());
    pack::GunPackRegistry empty; lua::GunLogic logic(empty, [](auto const&) {});
    auto data = dataFor(0); api = apiFor(data); CHECK(logic.startBolt(api));
    CHECK_FALSE(logic.tickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
}

TEST_CASE("Lua bolt nonboolean results and invalid hook types stop and warn once per gun and hook") {
    for (auto hook : {"start_bolt", "tick_bolt"}) for (auto value : {
        "function() return nil end", "function() end", "function() return 1 end",
        "function() return 'true' end", "function() return {} end", "42", "{}", "false", "'function'"}) {
        CAPTURE(hook);
        CAPTURE(value);
        BoltScripts fixture(std::string("return {[") + "'" + hook + "']=" + value + "}");
        auto api = fixture.api(); api.isBolting = true; api.boltNowMs = 1000;
        auto call = [&] { return std::string_view(hook) == "start_bolt" ? fixture.logic->startBolt(api) : fixture.logic->tickBolt(api); };
        CHECK_FALSE(call()); CHECK_FALSE(call()); CHECK(api.ammo == gun::GunAmmo{3, false});
        REQUIRE(fixture.warnings.size() == 1);
        CHECK(fixture.warnings[0].find(hook) != std::string::npos);
        CHECK(fixture.warnings[0].find("fixture:gun") != std::string::npos);
        api.gunId = {"fixture", "other"}; CHECK_FALSE(call()); CHECK(fixture.warnings.size() == 2);
    }
}

TEST_CASE("Lua bolt errors preserve preceding mutations and warn once separately for each hook") {
    BoltScripts fixture(R"(
        local function fail(api)
            api:setAmmoInBarrel(true)
            api:adjustBoltTime(25)
            error('synthetic failure')
        end
        return {start_bolt=fail, tick_bolt=fail}
    )");
    auto api = fixture.api(); api.boltTimestamp = 1000;
    CHECK_FALSE(fixture.logic->startBolt(api)); CHECK(api.ammo.barrel); CHECK(api.boltTimestamp == 975);
    CHECK_FALSE(fixture.logic->startBolt(api)); CHECK(fixture.warnings.size() == 1);
    api.isBolting = true;
    CHECK_FALSE(fixture.logic->tickBolt(api)); CHECK_FALSE(fixture.logic->tickBolt(api));
    CHECK(fixture.warnings.size() == 2);
    CHECK(api.ammo.barrel); CHECK(api.boltTimestamp == 900);
}

TEST_CASE("Start bolt false commits chamber ammo and adjusted timestamp without starting a tick") {
    BoltScripts fixture(R"(
        return {start_bolt=function(api)
            assert(api:getBoltTime() == 0)
            api:adjustBoltTime(75)
            assert(api:getBoltTime() == 0)
            if api:removeAmmoFromMagazine(1) == 1 then api:setAmmoInBarrel(true) end
            return false
        end}
    )");
    gun::GunAmmo item{3, false}; int64_t serverTimestamp = 1000; bool serverBolting = false;
    auto api = fixture.api(); api.ammo = item; api.boltTimestamp = serverTimestamp;
    api.isBolting = serverBolting;
    serverBolting = fixture.logic->startBolt(api);
    serverTimestamp = api.boltTimestamp; item = api.ammo; // unconditional server write-back
    CHECK_FALSE(serverBolting); CHECK(item == gun::GunAmmo{2, true}); CHECK(serverTimestamp == 925);
    CHECK(fixture.warnings.empty());
}

TEST_CASE("Lua adjustBoltTime writes through the timestamp for subsequent getters and server calls") {
    BoltScripts fixture(R"(
        return {
            start_bolt=function(api) api:adjustBoltTime(25); return true end,
            tick_bolt=function(api)
                local before = api:getBoltTime()
                api:adjustBoltTime(100)
                assert(api:getBoltTime() == before + 100)
                api:adjustBoltTime(-30)
                assert(api:getBoltTime() == before + 70)
                return true
            end
        }
    )");
    int64_t now = 1000, serverTimestamp = now;
    auto api = fixture.api(); api.boltTimestamp = serverTimestamp; api.boltClock = [&] { return now; };
    bool active = fixture.logic->startBolt(api); serverTimestamp = api.boltTimestamp;
    REQUIRE(active); CHECK(serverTimestamp == 975);
    now = 1100; api = fixture.api(); api.isBolting = active;
    api.boltTimestamp = serverTimestamp; api.boltClock = [&] { return now; };
    CHECK(api.getBoltTime() == 125); active = fixture.logic->tickBolt(api); serverTimestamp = api.boltTimestamp;
    CHECK(active); CHECK(serverTimestamp == 905); CHECK(api.getBoltTime() == 195);
    now = 1150; auto next = fixture.api(); next.isBolting = active;
    next.boltTimestamp = serverTimestamp; next.boltClock = [&] { return now; };
    CHECK(next.getBoltTime() == 245); CHECK(fixture.warnings.empty());
}

TEST_CASE("Lua getBoltTime samples the live clock per read and Java long timestamp arithmetic wraps") {
    BoltScripts fixture(R"(
        return {tick_bolt=function(api)
            local a = api:getBoltTime()
            local b = api:getBoltTime()
            assert(b == a + 1)
            return true
        end}
    )");
    auto api = fixture.api(); api.isBolting = true; api.boltTimestamp = 1000;
    int64_t now = 1100; api.boltClock = [&] { return now++; };
    CHECK(fixture.logic->tickBolt(api)); CHECK(fixture.warnings.empty());
    api.boltClock = {}; api.boltTimestamp = std::numeric_limits<int64_t>::min();
    api.adjustBoltTime(1); CHECK(api.boltTimestamp == std::numeric_limits<int64_t>::max());
    api.boltNowMs = std::numeric_limits<int64_t>::min(); CHECK(api.getBoltTime() == 1);
}

TEST_CASE("Lua bolt lookup errors and runaway hooks are protected and instruction budgets reset") {
    for (auto source : {
        "return setmetatable({}, {__index=function() error('lookup failed') end})",
        "return setmetatable({}, {__index=function() while true do end end})",
        "local function spin() while true do end end; return {start_bolt=spin, tick_bolt=spin}"}) {
        BoltScripts fixture(source); auto api = fixture.api();
        CHECK_FALSE(fixture.logic->startBolt(api)); CHECK_FALSE(fixture.logic->startBolt(api));
        CHECK_FALSE(fixture.logic->tickBolt(api)); CHECK_FALSE(fixture.logic->tickBolt(api));
        CHECK(fixture.warnings.size() == 2);
    }
}
