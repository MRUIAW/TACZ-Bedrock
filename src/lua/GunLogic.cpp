#include "lua/GunLogic.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <type_traits>
#include <utility>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>
#include "lua/BooleanCoercion.h"
#include "lua/NumericCoercion.h"

namespace tacz::lua {

namespace {

template <class R, class... Args>
auto live(R (ScriptApi::*fn)(Args...)) {
    return [fn](ScriptApi& a, Args... args) -> R {
        a.refreshView();
        if constexpr (std::is_void_v<R>) { (a.*fn)(args...); a.commitView(); }
        else { auto result = (a.*fn)(args...); a.commitView(); return result; }
    };
}
template <class R, class... Args>
auto live(R (ScriptApi::*fn)(Args...) const) {
    return [fn](ScriptApi& a, Args... args) -> R {
        a.refreshView();
        auto result = (a.*fn)(args...);
        return result;
    };
}

// A runaway script (an endless loop in a pack) must not freeze the server tick.
constexpr int InstructionLimit = 10'000'000;
constexpr int HookInterval     = 100'000;

void countHook(lua_State* L, lua_Debug*) {
    auto& budget = **static_cast<int**>(lua_getextraspace(L));
    budget -= HookInterval;
    if (budget <= 0) luaL_error(L, "script exceeded %d instructions", InstructionLimit);
}

// ScriptManager.getModuleName: "<namespace>_<path>".
std::string moduleName(pack::ResourceId const& id) { return id.ns + "_" + id.path; }

sol::object toLua(sol::state_view lua, nlohmann::json const& j) {
    switch (j.type()) {
    case nlohmann::json::value_t::boolean:
        return sol::make_object(lua, j.get<bool>());
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
        return sol::make_object(lua, j.get<int64_t>());
    case nlohmann::json::value_t::number_float:
        return sol::make_object(lua, j.get<double>());
    case nlohmann::json::value_t::string:
        return sol::make_object(lua, j.get<std::string>());
    case nlohmann::json::value_t::array: {
        auto table = lua.create_table();
        int  i     = 1;
        for (auto const& v : j) table[i++] = toLua(lua, v);
        return table;
    }
    case nlohmann::json::value_t::object: {
        auto table = lua.create_table();
        for (auto const& [k, v] : j.items()) table[k] = toLua(lua, v);
        return table;
    }
    default:
        return sol::lua_nil;
    }
}

} // namespace

struct GunLogic::Impl {
    pack::GunPackRegistry const& packs;
    LogFn                        warn;
    int                          instructionBudget = 0; // per VM, including independent test VMs
    sol::state                   lua;
    sol::protected_function      boltDispatch;
    sol::protected_function      voidDispatch;
    sol::protected_function      asyncDispatch;

    std::unordered_map<std::string, std::optional<sol::table>> scripts;      // by module name
    std::map<pack::ResourceId, sol::table>                     scriptParams; // GunData.script_param
    std::unordered_map<int64_t, sol::object>                   scriptData;   // by shooter
    std::set<std::string>                                      warned;
    gun::CycleTasks::Clock clock;
    std::unordered_map<int64_t, std::unique_ptr<gun::CycleTasks>> tasks;

    Impl(pack::GunPackRegistry const& p, LogFn w, gun::CycleTasks::Clock c)
    : packs(p), warn(std::move(w)), clock(std::move(c)) {
        if (!clock) clock = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count(); };
        initGlobals();
    }

    void warnOnce(std::string const& key, std::string const& message) {
        if (warned.insert(key).second) warn(message);
    }

    // ScriptManager.secureStandardGlobals: base, package, table, string, math (LuaJ also had
    // bit32, which Lua 5.4 dropped; the default pack does not use it).
    void initGlobals() {
        *static_cast<int**>(lua_getextraspace(lua.lua_state())) = &instructionBudget;
        lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::math);
        lua["dofile"]       = sol::lua_nil;
        lua["loadfile"]     = sol::lua_nil;
        lua["string"]["dump"] = sol::lua_nil;
        // Text chunks only: no precompiled bytecode from packs.
        sol::function rawLoad = lua["load"];
        lua["load"] = [rawLoad](sol::object chunk, sol::optional<std::string> name, sol::variadic_args) {
            return rawLoad(chunk, name.value_or("=(load)"), "t");
        };
        sol::table package = lua["package"];
        package["path"]    = "";
        package["cpath"]   = "";
        package["loadlib"] = sol::lua_nil;
        sol::table searchers = package["searchers"];
        sol::object preloadSearcher = searchers[1];
        package["searchers"] = lua.create_table_with(1, preloadSearcher);

        // LuaGunLogicConstant: ReloadState.StateType and FireMode ordinals.
        for (int i = 0; i <= 4; ++i) lua[gun::reloadStateName(static_cast<gun::ReloadStateType>(i))] = i;
        lua["AUTO"]    = 0;
        lua["SEMI"]    = 1;
        lua["BURST"]   = 2;
        lua["UNKNOWN"] = 3;

        registerApi();

        // Protect lookup too: __index may execute Lua or throw. Nil alone means
        // absent; checkFunction/checkboolean reject all other invalid types.
        boltDispatch = lua.safe_script(R"(
            local type, error = type, error
            return function(api, module, name)
                local fn = module[name]
                if fn == nil then return nil end
                if type(fn) ~= 'function' then error(name .. ': function or nil expected') end
                local value = fn(api)
                if type(value) ~= 'boolean' then error(name .. ': boolean result expected') end
                return value
            end
        )", sol::script_pass_on_error).get<sol::protected_function>();

        voidDispatch = lua.safe_script(R"(
            local type, error = type, error
            return function(api, module, name, timestamp)
                local fn = module[name]
                if fn == nil then return false end
                if type(fn) ~= 'function' then error(name .. ': function or nil expected') end
                if name == 'tick_heat' then fn(api, timestamp) else fn(api) end
                return true
            end
        )", sol::script_pass_on_error).get<sol::protected_function>();
        asyncDispatch = lua.safe_script(R"(
            local type, error = type, error
            return function(fn)
                local result = fn()
                if type(result) ~= 'boolean' then error('safeAsyncTask: boolean result expected') end
                return result
            end
        )", sol::script_pass_on_error).get<sol::protected_function>();

        sol::table preload = package["preload"];
        for (auto const& [id, source] : packs.dataScripts()) {
            std::string name = moduleName(id);
            preload[name]    = [this, name, source = source](sol::variadic_args) -> sol::object {
                auto result = lua.safe_script(source, sol::script_pass_on_error, "@" + name, sol::load_mode::text);
                if (!result.valid()) {
                    sol::error err = result;
                    warnOnce("load:" + name, "Failed to load script " + name + ": " + err.what());
                    return sol::lua_nil;
                }
                return result.get<sol::object>();
            };
        }
        lua_sethook(lua.lua_state(), countHook, LUA_MASKCOUNT, HookInterval);
    }

    void registerApi() {
        lua.new_usertype<ScriptApi>(
            "GunScriptApi",
            sol::no_constructor,
            "getReloadTime",
            [](ScriptApi& a) { a.refreshView(); return std::max<int64_t>(a.reloadTimeMs, 0); },
            "getReloadStateType",
            [](ScriptApi& a) { a.refreshView(); return static_cast<int>(a.reloadState); },
            "getNeededAmmoAmount",
            live(&ScriptApi::getNeededAmmoAmount),
            "getAmmoAmount",
            live(&ScriptApi::getAmmoAmount),
            "getAmmoCountInMagazine",
            live(&ScriptApi::getAmmoAmount),
            "getMaxAmmoCount",
            live(&ScriptApi::getMaxAmmoCount),
            "getMagExtentLevel",
            live(&ScriptApi::getMagExtentLevel),
            "hasAmmoInBarrel",
            live(&ScriptApi::hasAmmoInBarrel),
            "setAmmoInBarrel",
            [](ScriptApi& a, sol::object value) { a.refreshView(); a.setAmmoInBarrel(luaJBoolean(value)); a.commitView(); },
            "putAmmoInMagazine",
            live(&ScriptApi::putAmmoInMagazine),
            "removeAmmoFromMagazine",
            live(&ScriptApi::removeAmmoFromMagazine),
            "consumeAmmoFromPlayer",
            live(&ScriptApi::consumeAmmoFromPlayer),
            "hasAmmoToConsume",
            live(&ScriptApi::hasAmmoToConsume),
            "isReloadingNeedConsumeAmmo",
            [](ScriptApi& a) { a.refreshView(); return a.needCheckAmmo; },
            "isShootingNeedConsumeAmmo",
            [](ScriptApi& a) { a.refreshView(); return a.consumesAmmo; },
            "useInventoryAmmo",
            live(&ScriptApi::useInventoryAmmo),
            "getFireMode",
            live(&ScriptApi::getFireMode),
            "getAimingProgress",
            [](ScriptApi& a) { a.refreshView(); return a.aimingProgress; },
            "getCurrentTimestamp",
            &ScriptApi::getCurrentTimestamp,
            "getShootInterval",
            live(&ScriptApi::getShootInterval),
            "getLastShootTimestamp",
            &ScriptApi::getLastShootTimestamp,
            "adjustShootInterval",
            [](ScriptApi& a, sol::object alpha) { a.adjustShootInterval(luaJLong(alpha)); },
            "shootOnce",
            [this](ScriptApi& a, sol::object consume) { shootOnce(a, luaJBoolean(consume)); },
            "safeAsyncTask",
            [this](ScriptApi& a, sol::object fn, sol::object delay, sol::object period, sol::object cycles) {
                if (fn.get_type() != sol::type::function) throw sol::error("safeAsyncTask: function expected");
                auto callback = fn.as<sol::protected_function>();
                auto owned = std::make_shared<ScriptApi>(a);
                schedule(a, [this, owned, callback] {
                    if (owned->alive && !owned->alive()) return false;
                    owned->refreshView();
                    instructionBudget = InstructionLimit;
                    auto result = asyncDispatch(callback);
                    if (!result.valid()) { report(*owned, "safe_async_task", sol::error(result).what()); return false; }
                    return result.get<bool>();
                }, luaJLong(delay), luaJLong(period), luaJInt(cycles), gun::CycleTasks::Kind::Async);
            },
            "getBoltTime",
            live(&ScriptApi::getBoltTime),
            "adjustBoltTime",
            live(&ScriptApi::adjustBoltTime),
            "getAttachment",
            live(&ScriptApi::getAttachment),
            "getScriptParams",
            [this](ScriptApi& a, sol::this_state) -> sol::table { return paramsFor(a); },
            "cacheScriptData",
            [this](ScriptApi& a, sol::object value) { scriptData[a.shooterKey] = value; },
            "getCachedScriptData",
            [this](ScriptApi& a, sol::this_state s) -> sol::object {
                auto it = scriptData.find(a.shooterKey);
                return it == scriptData.end() ? sol::make_object(s, sol::lua_nil) : it->second;
            }
        );
    }

    sol::table paramsFor(ScriptApi const& a) {
        auto it = scriptParams.find(a.gunId);
        if (it != scriptParams.end()) return it->second;
        sol::object value = a.data->scriptParam.is_object() ? toLua(lua, a.data->scriptParam) : sol::make_object(lua, lua.create_table());
        sol::table  table = value.as<sol::table>();
        scriptParams.emplace(a.gunId, table);
        return table;
    }

    std::optional<sol::table> script(pack::ResourceId const& id) {
        std::string name = moduleName(id);
        if (auto it = scripts.find(name); it != scripts.end()) return it->second;
        std::optional<sol::table> table;
        if (packs.dataScripts().contains(id)) {
            instructionBudget = InstructionLimit;
            sol::protected_function require = lua["require"];
            auto                    result  = require(name);
            if (!result.valid()) {
                sol::error err = result;
                warnOnce("require:" + name, "Failed to load script " + name + ": " + err.what());
            } else if (result.get_type() == sol::type::table) {
                table = result.get<sol::table>();
            } else {
                warnOnce("require:" + name, "Script " + name + " does not return a table");
            }
        }
        scripts.emplace(name, table);
        return table;
    }

    // The gun's script function, or nothing if the gun has no script or the script lacks it.
    std::optional<sol::protected_function> function(ScriptApi const& a, char const* name) {
        if (!a.data->script) return std::nullopt;
        auto table = script(*a.data->script);
        if (!table) return std::nullopt;
        sol::object fn = (*table)[name];
        if (fn.get_type() != sol::type::function) return std::nullopt;
        return fn.as<sol::protected_function>();
    }

    template <class... Args>
    sol::protected_function_result call(ScriptApi& a, sol::protected_function const& fn, char const* name, Args&&... args) {
        instructionBudget = InstructionLimit;
        auto owned = std::make_shared<ScriptApi>(a);
        auto result = fn(owned, std::forward<Args>(args)...);
        a = *owned;
        if (!result.valid()) {
            sol::error err = result;
            warnOnce(
                std::string(name) + ":" + a.gunId.toString(),
                "Script " + a.data->script->toString() + " " + name + " failed for " + a.gunId.toString() + ": " + err.what()
            );
        }
        return result;
    }

    bool bolt(ScriptApi& a, char const* name, bool start) {
        auto table = a.data->script ? script(*a.data->script) : std::nullopt;
        if (!table) return start || defaultTickBolt(a);
        auto result = call(a, boltDispatch, name, *table, name);
        if (!result.valid()) return false;
        if (result.get_type() == sol::type::nil) return start || defaultTickBolt(a);
        return result.get<bool>();
    }

    void report(ScriptApi const& a, char const* name, std::string const& error) {
        warnOnce(std::string(name) + ":" + a.gunId.toString(), "Script " +
            (a.data->script ? a.data->script->toString() : "<default>") + " " + name + " failed for " + a.gunId.toString() + ": " + error);
    }
    gun::CycleTasks& scheduler(int64_t key) {
        auto& ptr = tasks[key];
        if (!ptr) ptr = std::make_unique<gun::CycleTasks>(clock);
        return *ptr;
    }
    void schedule(ScriptApi& a, gun::CycleTasks::Task task, int64_t delay, int64_t period, int cycles, gun::CycleTasks::Kind kind) {
        if (!scheduler(a.shooterKey).add(std::move(task), delay, period, cycles, kind))
            report(a, "task_limit", "256 scheduled tasks per player exceeded");
    }
    void shootOnce(ScriptApi& a, bool consume) {
        a.refreshView();
        // Java checks life/hold inside the immediate first cycle, not here.
        auto mode = a.fireMode;
        auto cycles = mode == pack::FireMode::Burst ? a.data->burst.count : 1;
        auto period = mode == pack::FireMode::Burst ? gun::burstShootIntervalMs(*a.data) : 1;
        auto body = a.prepareRound ? a.prepareRound(a, consume) : gun::CycleTasks::Task{};
        if (!body) return;
        schedule(a, std::move(body), 0, period, cycles, gun::CycleTasks::Kind::Shot);
        a.refreshView(); // immediate cycle's ammo is visible to the next script statement
    }
    void dispatchVoid(ScriptApi& a, char const* name, int64_t timestamp, bool shoot) {
        auto table = a.data->script ? script(*a.data->script) : std::nullopt;
        if (!table) { if (shoot) shootOnce(a, a.consumesAmmo); return; }
        auto result = call(a, voidDispatch, name, *table, name, timestamp);
        if (result.valid() && !result.get<bool>() && shoot) shootOnce(a, a.consumesAmmo);
    }
};

GunLogic::GunLogic(pack::GunPackRegistry const& packs, LogFn warn, gun::CycleTasks::Clock clock)
: mImpl(std::make_unique<Impl>(packs, std::move(warn), std::move(clock))) {}

GunLogic::~GunLogic() = default;

bool GunLogic::startReload(ScriptApi& api) {
    api.logic = this;
    auto fn   = mImpl->function(api, "start_reload");
    if (!fn) return true;
    auto result = mImpl->call(api, *fn, "start_reload");
    // LuaJ checkboolean: anything but a boolean is an error, which cancels the reload.
    return result.valid() && result.get_type() == sol::type::boolean && result.get<bool>();
}

gun::ReloadState GunLogic::tickReload(ScriptApi& api) {
    api.logic = this;
    auto fn   = mImpl->function(api, "tick_reload");
    if (!fn) return defaultTickReload(api);
    auto result = mImpl->call(api, *fn, "tick_reload");
    if (!result.valid() || result.return_count() < 2) return {};
    // LuaJ checkint / checklong truncate numbers.
    sol::object typeValue  = result[0];
    sol::object countValue = result[1];
    if (typeValue.get_type() != sol::type::number || countValue.get_type() != sol::type::number) return {};
    auto type = gun::reloadStateFromOrdinal(static_cast<int>(std::trunc(typeValue.as<double>())));
    if (!type) return {};
    return {*type, static_cast<int64_t>(std::trunc(countValue.as<double>()))};
}

void GunLogic::interruptReload(ScriptApi& api) {
    api.logic = this;
    if (auto fn = mImpl->function(api, "interrupt_reload")) mImpl->call(api, *fn, "interrupt_reload");
}

void GunLogic::clearScriptData(int64_t shooterKey) { mImpl->scriptData.erase(shooterKey); }

bool GunLogic::loadScript(pack::ResourceId const& id) { return mImpl->script(id).has_value(); }

bool GunLogic::startBolt(ScriptApi& api) {
    api.logic = this;
    return mImpl->bolt(api, "start_bolt", true);
}

bool GunLogic::tickBolt(ScriptApi& api) {
    api.logic = this;
    return mImpl->bolt(api, "tick_bolt", false);
}

void GunLogic::shoot(ScriptApi& api) { api.logic = this; mImpl->dispatchVoid(api, "shoot", 0, true); }
void GunLogic::tickHeat(ScriptApi& api, int64_t timestamp) { api.logic = this; mImpl->dispatchVoid(api, "tick_heat", timestamp, false); }
void GunLogic::tickTasks(int64_t key) {
    auto found = mImpl->tasks.find(key);
    if (found != mImpl->tasks.end()) found->second->tick();
}
void GunLogic::clearTasks(int64_t key) {
    auto found = mImpl->tasks.find(key);
    if (found != mImpl->tasks.end()) found->second->clear();
}
void GunLogic::removeShooter(int64_t key) {
    mImpl->tasks.erase(key);
    mImpl->scriptData.erase(key);
}
void GunLogic::cancelShots(int64_t key) {
    auto found = mImpl->tasks.find(key);
    if (found != mImpl->tasks.end()) found->second->cancelShots();
}
bool GunLogic::pendingShots(int64_t key) const {
    auto found = mImpl->tasks.find(key);
    return found != mImpl->tasks.end() && found->second->pendingShots();
}

} // namespace tacz::lua
