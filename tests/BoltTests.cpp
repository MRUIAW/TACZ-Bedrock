#include <doctest/doctest.h>

#include <future>
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
}

TEST_CASE("Default bolt feeds once at feed time and remains active until action time") {
    auto data = dataFor(.9f, .3f); auto api = apiFor(data);
    api.boltTimeMs = 299; CHECK(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{3, false});
    api.boltTimeMs = 300; CHECK(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    api.boltTimeMs = 899; CHECK(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    api.boltTimeMs = 900; CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    api.boltTimeMs = 950; CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
}

TEST_CASE("Default bolt exact minus one feed means action while other negative feed times do not") {
    auto data = dataFor(.57f); auto api = apiFor(data);
    CHECK(gun::boltMillis(.57f) == 570);
    api.boltTimeMs = 569; CHECK(lua::defaultTickBolt(api)); CHECK_FALSE(api.ammo.barrel);
    api.boltTimeMs = 570; CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    for (float feed : {-.5f, -1.0001f}) {
        data.boltFeedTime = feed; api = apiFor(data);
        CHECK(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    }
}

TEST_CASE("Default bolt supports zero action no clip and feed later than action") {
    auto zero = dataFor(0); auto api = apiFor(zero);
    CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
    auto late = dataFor(.1f, .4f); api = apiFor(late);
    for (int64_t time : {0, 100, 399}) { api.boltTimeMs = time; CHECK(lua::defaultTickBolt(api)); CHECK_FALSE(api.ammo.barrel); }
    api.boltTimeMs = 400; CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{2, true});
}

TEST_CASE("Default bolt does not invent feed ammo or consume magazine ammo for an already chambered item") {
    auto data = dataFor(0);
    auto api = apiFor(data, {0, false}); CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{0, false});
    api = apiFor(data, {3, true}); CHECK_FALSE(lua::defaultTickBolt(api)); CHECK(api.ammo == gun::GunAmmo{3, true});
    data.boltActionTime = .5f; data.boltFeedTime = .1f;
    api = apiFor(data, {0, false}); api.boltTimeMs = 100;
    CHECK(lua::defaultTickBolt(api)); CHECK_FALSE(api.ammo.barrel);
    api.boltTimeMs = 500; CHECK_FALSE(lua::defaultTickBolt(api)); CHECK_FALSE(api.ammo.barrel);
}

TEST_CASE("Default inventory bolt consumes exactly one stock round or creative virtual round") {
    auto data = dataFor(.5f, .1f); data.reload.type = pack::FeedType::Inventory;
    for (bool creative : {false, true}) for (int stock : {0, 2}) {
        auto api = apiFor(data, {0, false}); api.needCheckAmmo = !creative;
        int original = stock, calls = 0;
        api.extractAmmo = [&](int n) { ++calls; int take = std::min(n, stock); stock -= take; return take; };
        api.boltTimeMs = 99; CHECK(lua::defaultTickBolt(api)); CHECK(calls == 0);
        api.boltTimeMs = 100; CHECK(lua::defaultTickBolt(api));
        CHECK(api.ammo.barrel == (creative || original > 0)); CHECK(api.ammo.magazine == 0);
        CHECK(stock == (creative || !original ? original : original - 1));
        CHECK(calls == (creative ? 0 : 1));
        if (api.ammo.barrel) { CHECK(lua::defaultTickBolt(api)); CHECK(calls == (creative ? 0 : 1)); }
    }
}

TEST_CASE("Bolt elapsed API is zero when inactive and otherwise preserves elapsed wall-clock milliseconds") {
    lua::ScriptApi api; api.boltTimeMs = 123;
    CHECK(api.getBoltTime() == 0);
    api.isBolting = true; CHECK(api.getBoltTime() == 123);
    api.boltTimeMs = -12; CHECK(api.getBoltTime() == -12);
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

TEST_CASE("Bolt refuses missing gun data display and staged hooks without starting an operation") {
    gun::BoltCheck c; c.ammo = {1, false}; CHECK_FALSE(gun::serverBoltRefusal(c)); CHECK_FALSE(gun::clientBoltRefusal(c));
    c.hasGun = false; CHECK(gun::serverBoltRefusal(c)); CHECK(gun::clientBoltRefusal(c)); c.hasGun = true;
    c.knownData = false; CHECK(gun::serverBoltRefusal(c)); CHECK(gun::clientBoltRefusal(c)); c.knownData = true;
    c.hasDisplay = false; CHECK_FALSE(gun::serverBoltRefusal(c)); CHECK(gun::clientBoltRefusal(c)); c.hasDisplay = true;
    c.scriptedHooks = true; CHECK(std::string_view(gun::serverBoltRefusal(c)) == "SCRIPT_BOLT_DEFERRED");
    CHECK(std::string_view(gun::clientBoltRefusal(c)) == "SCRIPT_BOLT_DEFERRED");
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
    auto data = dataFor(.8f, .2f); auto api = apiFor(data); api.boltTimeMs = 200;
    bool active = lua::defaultTickBolt(api); CHECK(active); CHECK(api.ammo.barrel);
    CHECK(gun::boltBlocksAction(active)); // same predicate wired to both server action handlers
    gun::sync::Snapshot state;
    state.set({gun::sync::Key::DrawCoolDown, int64_t{0}}); state.set({gun::sync::Key::MeleeCoolDown, int64_t{0}});
    state.set({gun::sync::Key::IsBolting, active}); CHECK_FALSE(gun::syncedShootPrecheck(state));
    gun::ClientStateLock lock; lock.lockState(gun::LockCondition::Bolt, 0);
    CHECK_FALSE(lock.tickStateLock(10, state)); CHECK(lock.clientStateLock);
    CHECK(lock.lockedCondition == gun::LockCondition::None);
    api.boltTimeMs = 800; active = lua::defaultTickBolt(api); CHECK_FALSE(gun::boltBlocksAction(active));
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

TEST_CASE("Script staging query uses executed module tables and inheritance not hook text matching") {
    auto source = std::make_unique<ScriptSource>();
    source->files["data/fixture/scripts/base.lua"] = "return {start_bolt=function() error('must not execute') end}";
    source->files["data/fixture/scripts/inherited.lua"] = "return setmetatable({}, {__index=require('fixture_base')})";
    source->files["data/fixture/scripts/dynamic.lua"] = "local t={}; t['tick_'..'bolt']=function() return true end; return t";
    source->files["data/fixture/scripts/reload.lua"] = "-- start_bolt is only a comment\nreturn {start_reload=function() return true end}";
    source->files["data/fixture/scripts/malformed.lua"] = "return {tick_bolt=0}";
    source->files["data/fixture/scripts/lookup.lua"] = "return setmetatable({}, {__index=function(_, key) local n=0; for i=1,100000 do n=n+i end; if key=='tick_bolt' then return function() return true end end end})";
    std::vector<std::unique_ptr<pack::PackSource>> sources; sources.push_back(std::move(source));
    pack::GunPackRegistry registry; registry.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    std::vector<std::string> warnings; lua::GunLogic query(registry, [&](auto const& w) { warnings.push_back(w); });
    auto data = dataFor(.5f);
    CHECK_FALSE(query.hasBoltHooks(data));
    for (auto name : {"base", "inherited", "dynamic", "malformed"}) {
        data.script = pack::ResourceId{"fixture", name}; CHECK(query.hasBoltHooks(data)); CHECK(query.hasBoltHooks(data));
    }
    data.script = pack::ResourceId{"fixture", "reload"}; CHECK_FALSE(query.hasBoltHooks(data));
    data.script = pack::ResourceId{"fixture", "lookup"};
    for (int attempt = 0; attempt < 30; ++attempt) CHECK(query.hasBoltHooks(data));
    CHECK(warnings.empty());
}

TEST_CASE("Client query and server reload VMs keep independent instruction budgets") {
    auto source = std::make_unique<ScriptSource>();
    source->files["data/fixture/scripts/work.lua"] = "local n=0; for i=1,500000 do n=n+i end; return {}";
    std::vector<std::unique_ptr<pack::PackSource>> sources; sources.push_back(std::move(source));
    pack::GunPackRegistry registry; registry.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    auto run = [&] { lua::GunLogic logic(registry, [](auto const&) {}); return logic.loadScript({"fixture", "work"}); };
    auto other = std::async(std::launch::async, run);
    CHECK(run()); CHECK(other.get());
}
