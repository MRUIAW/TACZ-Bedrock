#include "lua/GunLogic.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace tacz::lua {

namespace {

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
    int                          instructionBudget = 0; // per VM, not shared with client query VM
    sol::state                   lua;

    std::unordered_map<std::string, std::optional<sol::table>> scripts;      // by module name
    std::map<pack::ResourceId, sol::table>                     scriptParams; // GunData.script_param
    std::unordered_map<int64_t, sol::object>                   scriptData;   // by shooter
    std::set<std::string>                                      warned;

    Impl(pack::GunPackRegistry const& p, LogFn w) : packs(p), warn(std::move(w)) { initGlobals(); }

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
            [](ScriptApi& a) { return std::max<int64_t>(a.reloadTimeMs, 0); },
            "getReloadStateType",
            [](ScriptApi& a) { return static_cast<int>(a.reloadState); },
            "getNeededAmmoAmount",
            &ScriptApi::getNeededAmmoAmount,
            "getAmmoAmount",
            &ScriptApi::getAmmoAmount,
            "getAmmoCountInMagazine",
            &ScriptApi::getAmmoAmount,
            "getMaxAmmoCount",
            &ScriptApi::getMaxAmmoCount,
            "getMagExtentLevel",
            &ScriptApi::getMagExtentLevel,
            "hasAmmoInBarrel",
            &ScriptApi::hasAmmoInBarrel,
            "setAmmoInBarrel",
            &ScriptApi::setAmmoInBarrel,
            "putAmmoInMagazine",
            &ScriptApi::putAmmoInMagazine,
            "removeAmmoFromMagazine",
            &ScriptApi::removeAmmoFromMagazine,
            "consumeAmmoFromPlayer",
            &ScriptApi::consumeAmmoFromPlayer,
            "hasAmmoToConsume",
            &ScriptApi::hasAmmoToConsume,
            "isReloadingNeedConsumeAmmo",
            [](ScriptApi& a) { return a.needCheckAmmo; },
            "isShootingNeedConsumeAmmo",
            [](ScriptApi& a) { return a.consumesAmmo; },
            "useInventoryAmmo",
            &ScriptApi::useInventoryAmmo,
            "getFireMode",
            &ScriptApi::getFireMode,
            "getAimingProgress",
            [](ScriptApi& a) { return a.aimingProgress; },
            "getCurrentTimestamp",
            [](ScriptApi& a) { return a.nowMs; },
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
        auto result       = fn(std::ref(a), std::forward<Args>(args)...);
        if (!result.valid()) {
            sol::error err = result;
            warnOnce(
                std::string(name) + ":" + a.gunId.toString(),
                "Script " + a.data->script->toString() + " " + name + " failed for " + a.gunId.toString() + ": " + err.what()
            );
        }
        return result;
    }
};

GunLogic::GunLogic(pack::GunPackRegistry const& packs, LogFn warn)
: mImpl(std::make_unique<Impl>(packs, std::move(warn))) {}

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

bool GunLogic::hasBoltHooks(pack::GunData const& data) {
    if (!data.script) return false;
    auto table = mImpl->script(*data.script);
    if (!table) return false; // same missing/failed module policy as existing script loading
    mImpl->instructionBudget = InstructionLimit; // __index may itself be a Lua function
    // Any non-nil declaration is deferred: a malformed non-function must not
    // accidentally enable Java's default path (checkFunction rejects it in Java).
    sol::object start = (*table)["start_bolt"];
    sol::object tick = (*table)["tick_bolt"];
    return start.get_type() != sol::type::nil || tick.get_type() != sol::type::nil;
}

} // namespace tacz::lua
