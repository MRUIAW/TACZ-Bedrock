#include "lua/AnimationStateMachine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

#include "gun/Reload.h"

namespace tacz::lua {
namespace {
constexpr int InstructionLimit = 10'000'000;
constexpr int HookInterval = 10'000;

sol::object toLua(sol::state_view lua, nlohmann::json const& j) {
    if (j.is_boolean()) return sol::make_object(lua, j.get<bool>());
    if (j.is_number_integer()) return sol::make_object(lua, j.get<int64_t>());
    if (j.is_number()) return sol::make_object(lua, j.get<double>());
    if (j.is_string()) return sol::make_object(lua, j.get<std::string>());
    if (j.is_object() || j.is_array()) {
        sol::table table = lua.create_table();
        if (j.is_object()) for (auto const& [key, val] : j.items()) table[key] = toLua(lua, val);
        else { int i = 1; for (auto const& val : j) table[i++] = toLua(lua, val); }
        return table;
    }
    return sol::lua_nil;
}

// LuaJ's checkint/checklong truncate numbers. Do not let sol2's stricter Lua 5.4 integer
// conversion reject fractional millisecond adjustments used by third-party scripts.
int integer(double value) {
    if (!std::isfinite(value) || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        throw std::out_of_range("animation integer argument");
    }
    return static_cast<int>(value);
}
int64_t longInteger(double value) {
    if (!std::isfinite(value) || value < -9e18 || value > 9e18) throw std::out_of_range("animation timestamp argument");
    return static_cast<int64_t>(value);
}
animation::PlayType playType(double value) {
    switch (integer(value)) {
    case 0: return animation::PlayType::Hold;
    case 1: return animation::PlayType::Stop;
    case 2: return animation::PlayType::Loop;
    default: throw std::out_of_range("animation play type");
    }
}

struct NbtView {
    AnimationContext* context;
    std::vector<std::string> path;
    nlohmann::json read(std::string const& key, int mode) const {
        return context->readNbt ? context->readNbt(path, key, mode) : nlohmann::json{};
    }
    double number(std::string const& key) const { auto v = read(key, 1); return v.is_number() ? v.get<double>() : 0; }
};
} // namespace

struct AnimationStateMachine::Impl {
    pack::GunPackRegistry const& packs;
    pack::ResourceId scriptId;
    AnimationContext& context;
    animation::LogFn warn;
    animation::Controller::SoundFn sounds;
    animation::Controller controller;
    animation::Controller debugController;
    bool debugActive = false;
    bool initialized = false, failed = false;
    double now = 0;
    int budget = InstructionLimit, depth = 0;
    sol::state lua;
    sol::table script, params;
    std::vector<sol::table> states;

    Impl(pack::GunPackRegistry const& p, animation::AnimationFile const& a, pack::ResourceId id,
         AnimationContext& c, animation::LogFn w, animation::Controller::SoundFn s)
    : packs(p), scriptId(std::move(id)), context(c), warn(std::move(w)), sounds(std::move(s)),
      controller(a, [this](auto const& name, double from, double to) { if (!debugActive && sounds) sounds(name, from, to); }),
      debugController(a, [this](auto const& name, double from, double to) { if (sounds) sounds(name, from, to); }) {}

    static void hook(lua_State* state, lua_Debug*) {
        auto self = *static_cast<Impl**>(lua_getextraspace(state));
        self->budget -= HookInterval;
        if (self->budget <= 0) luaL_error(state, "animation script exceeded %d instructions", InstructionLimit);
    }

    void fallback(std::string const& error) {
        if (!failed && warn) warn("Animation script " + scriptId.toString() + ": " + error + "; falling back to static_idle");
        failed = true;
        states.clear();
        debugActive = false;
        context.hideCrossHair = false;
        controller.clear();
        controller.setTime(now);
        controller.addTrackLine();
        auto track = controller.assignNewTrack(0);
        controller.runAnimation("static_idle", track, false, animation::PlayType::Loop, 0);
    }

    template <class F> bool protect(F&& operation) {
        if (failed) return false;
        budget = InstructionLimit;
        try { operation(); return true; }
        catch (std::exception const& e) { fallback(e.what()); }
        catch (...) { fallback("unknown native error"); }
        return false;
    }

    template <class... Args> sol::object call(sol::table const& table, char const* name, Args&&... args) {
        sol::object fn = table[name];
        if (fn == sol::lua_nil) return sol::make_object(lua, sol::lua_nil);
        if (fn.get_type() != sol::type::function) throw std::runtime_error(std::string(name) + " must be a function or nil");
        if (++depth > 64) { --depth; throw std::runtime_error("animation callback recursion limit"); }
        sol::protected_function function = fn;
        auto result = function(script, std::forward<Args>(args)...);
        --depth;
        if (!result.valid()) { sol::error error = result; throw std::runtime_error(std::string(name) + ": " + error.what()); }
        return result.return_count() == 0 ? sol::make_object(lua, sol::lua_nil) : result.template get<sol::object>();
    }

    sol::table captureState(sol::table const& state) {
        // LuaAnimationState caches these four function references when it is constructed;
        // later mutation of the source table must not replace a live state's callbacks.
        auto captured = lua.create_table();
        for (char const* name : {"entry", "update", "exit", "transition"}) {
            sol::object v = state[name];
            if (v != sol::lua_nil && v.get_type() != sol::type::function) throw std::runtime_error(std::string(name) + " must be function or nil");
            captured[name] = v;
        }
        return captured;
    }

    void trigger(std::string const& input) {
        if (!initialized) return;
        // Index iteration intentionally reads the live list: nested context:trigger inputs
        // may replace other states during entry/update (AnimationStateMachine.trigger).
        for (size_t i = 0; i < states.size(); ++i) {
            auto state = states[i];
            auto next = call(state, "transition", std::ref(context), input);
            if (next == sol::lua_nil) continue;
            if (next.get_type() != sol::type::table) throw std::runtime_error("transition must return a table or nil");
            auto target = captureState(next.as<sol::table>());
            call(state, "exit", std::ref(context));
            states[i] = target;
            call(target, "entry", std::ref(context));
        }
    }

    void globals() {
        lua.open_libraries(sol::lib::base, sol::lib::package, sol::lib::table, sol::lib::string, sol::lib::math);
        lua["dofile"] = lua["loadfile"] = sol::lua_nil;
        lua["string"]["dump"] = sol::lua_nil;
        sol::function rawLoad = lua["load"];
        lua["load"] = [rawLoad](sol::object chunk, sol::optional<std::string> name, sol::variadic_args) {
            return rawLoad(chunk, name.value_or("=(load)"), "t");
        };
        sol::table package = lua["package"];
        package["path"] = package["cpath"] = "";
        package["loadlib"] = sol::lua_nil;
        sol::object preloadSearcher = package["searchers"][1];
        package["searchers"] = lua.create_table_with(1, preloadSearcher);
        // Compatibility helpers only; pack scripts are kept byte-for-byte unchanged. LuaJ
        // has unpack/math.pow and formats integral numbers without a '.0' suffix.
        auto shim = lua.safe_script(R"(
            unpack = table.unpack
            math.pow = function(x,y) return x^y end
            loadstring = function(s,n) return load(s,n,'t') end
            local original_tostring = tostring
            tostring = function(v)
                if type(v)=='number' and v==math.floor(v) and math.abs(v)<1e16 then
                    return string.format('%.0f',v)
                end
                return original_tostring(v)
            end
            package.loaders = package.searchers
            bit32 = {}
            local function u(x) return math.floor(x) & 0xffffffff end
            function bit32.band(...) local r=0xffffffff; for _,v in ipairs({...}) do r=r & u(v) end; return r end
            function bit32.bor(...) local r=0; for _,v in ipairs({...}) do r=r | u(v) end; return r end
            function bit32.bxor(...) local r=0; for _,v in ipairs({...}) do r=r ~ u(v) end; return r end
            function bit32.bnot(x) return (~u(x)) & 0xffffffff end
            function bit32.btest(...) return bit32.band(...)~=0 end
            function bit32.lshift(x,n) n=math.floor(n); if n<0 then return bit32.rshift(x,-n) end; if n>=32 then return 0 end; return (u(x)<<n)&0xffffffff end
            function bit32.rshift(x,n) n=math.floor(n); if n<0 then return bit32.lshift(x,-n) end; if n>=32 then return 0 end; return u(x)>>n end
            function bit32.arshift(x,n) n=math.floor(n); x=u(x); if n<0 then return bit32.lshift(x,-n) end; if n>=32 then return x>=0x80000000 and 0xffffffff or 0 end; return u(math.floor((x>=0x80000000 and x-0x100000000 or x)/2^n)) end
            function bit32.lrotate(x,n) n=math.floor(n)%32; return (bit32.lshift(x,n)|bit32.rshift(x,32-n))&0xffffffff end
            function bit32.rrotate(x,n) return bit32.lrotate(x,-n) end
            function bit32.extract(x,f,w) w=w or 1; assert(f>=0 and w>0 and f+w<=32); return (u(x)>>f)&((1<<w)-1) end
            function bit32.replace(x,v,f,w) w=w or 1; assert(f>=0 and w>0 and f+w<=32); local m=((1<<w)-1)<<f; return ((u(x)&(~m))|((u(v)<<f)&m))&0xffffffff end
        )", sol::script_pass_on_error, "@tacz_luaj_compat", sol::load_mode::text);
        if (!shim.valid()) { sol::error error = shim; throw std::runtime_error(error.what()); }
        lua["print"] = [this](sol::variadic_args args) {
            if (!context.print) return;
            std::string message = "[anim:" + scriptId.toString() + "]";
            sol::protected_function tostring = lua["tostring"];
            for (sol::object arg : args) {
                auto result = tostring(arg);
                if (!result.valid()) { sol::error error = result; throw std::runtime_error(error.what()); }
                message += " " + result.get<std::string>();
                if (message.size() > 4096) { message.resize(4096); break; }
            }
            context.print(message);
        };
        lua["PLAY_ONCE_HOLD"] = 0;
        lua["PLAY_ONCE_STOP"] = 1;
        lua["LOOP"] = 2;
        for (auto const& [key, val] : std::initializer_list<std::pair<char const*, char const*>>{
            {"INPUT_BOLT","blot"}, {"INPUT_DRAW","draw"}, {"INPUT_PUT_AWAY","put_away"}, {"INPUT_FIRE_SELECT","fire_select"},
            {"INPUT_INSPECT","inspect"}, {"INPUT_BAYONET_MUZZLE","bayonet_muzzle"}, {"INPUT_BAYONET_STOCK","bayonet_stock"},
            {"INPUT_BAYONET_PUSH","bayonet_push"}, {"INPUT_RELOAD","reload"}, {"INPUT_CANCEL_RELOAD","cancel_reload"},
            {"INPUT_SHOOT","shoot"}, {"INPUT_WALK","walk"}, {"INPUT_RUN","run"}, {"INPUT_IDLE","idle"}}) lua[key] = val;
        for (int i = 0; i <= 4; ++i) lua[gun::reloadStateName(static_cast<gun::ReloadStateType>(i))] = i;
        lua["AUTO"] = 0; lua["SEMI"] = 1; lua["BURST"] = 2; lua["UNKNOWN"] = 3;
        api();
        params = toLua(lua, context.params.is_object() ? context.params : nlohmann::json::object()).as<sol::table>();
        sol::table preload = package["preload"];
        for (auto const& [id, source] : packs.assetScripts()) {
            std::string name = id.ns + "_" + id.path;
            preload[name] = [this, name, source](sol::variadic_args) -> sol::object {
                auto result = lua.safe_script(source, sol::script_pass_on_error, "@" + name, sol::load_mode::text);
                if (!result.valid()) { sol::error error = result; throw std::runtime_error("module " + name + ": " + error.what()); }
                if (result.get_type() != sol::type::table) throw std::runtime_error("module " + name + " must return a table");
                return result.get<sol::object>();
            };
        }
        *static_cast<Impl**>(lua_getextraspace(lua.lua_state())) = this;
        lua_sethook(lua.lua_state(), hook, LUA_MASKCOUNT, HookInterval);
    }

    void api() {
        auto a = lua.new_usertype<AnimationContext>("GunAnimationStateContext", sol::no_constructor);
        a["addTrackLine"] = [this](AnimationContext&) { return controller.addTrackLine(); };
        a["ensureTrackLineSize"] = [this](AnimationContext&, double n) { controller.ensureTrackLineSize(integer(n)); };
        a["getTrackLineSize"] = [this](AnimationContext&) { return controller.getTrackLineSize(); };
        a["assignNewTrack"] = [this](AnimationContext&, double l) { return controller.assignNewTrack(integer(l)); };
        a["findIdleTrack"] = [this](AnimationContext&, double l, bool interrupt) { return controller.findIdleTrack(integer(l), interrupt); };
        a["ensureTracksAmount"] = [this](AnimationContext&, double l, double n) { controller.ensureTracksAmount(integer(l), integer(n)); };
        a["getTrack"] = [this](AnimationContext&, double l, double n) { return controller.getTrack(integer(l), integer(n)); };
        a["getAsSingletonTrack"] = [this](AnimationContext&, double l) { return controller.getAsSingletonTrack(integer(l)); };
        a["runAnimation"] = [this](AnimationContext&, std::string name, double t, bool blend, double type, float transition) {
            controller.runAnimation(name, integer(t), blend, playType(type), transition);
        };
        a["stopAnimation"] = [this](AnimationContext&, double t) { controller.stopAnimation(integer(t)); };
        a["holdAnimation"] = [this](AnimationContext&, double t) { controller.holdAnimation(integer(t)); };
        a["pauseAnimation"] = [this](AnimationContext&, double t) { controller.pauseAnimation(integer(t)); };
        a["resumeAnimation"] = [this](AnimationContext&, double t) { controller.resumeAnimation(integer(t)); };
        a["setAnimationProgress"] = [this](AnimationContext&, double t, float p, bool n) { controller.setAnimationProgress(integer(t), p, n); };
        a["adjustAnimationProgress"] = [this](AnimationContext&, double t, float p, bool n) { controller.adjustAnimationProgress(integer(t), p, n); };
        a["isHolding"] = [this](AnimationContext&, double t) { return controller.info(integer(t)).holding; };
        a["isStopped"] = [this](AnimationContext&, double t) { return controller.info(integer(t)).stopped; };
        // Preserve AnimationStateContext.isPause's (surprising) !isPausing return value.
        a["isPause"] = [this](AnimationContext&, double t) { auto i = controller.info(integer(t)); return !i.name.empty() && !i.pausing; };
        a["hasAnimationPrototype"] = [this](AnimationContext&, std::string const& name) { return controller.hasAnimationPrototype(name); };
        a["trigger"] = [this](AnimationContext&, std::string const& input) { trigger(input); };
        a["shouldHideCrossHair"] = [](AnimationContext& c) { return c.hideCrossHair; };
        a["setShouldHideCrossHair"] = [](AnimationContext& c, bool hide) { c.hideCrossHair = hide; };
        a["hasBulletInBarrel"] = [](AnimationContext& c) { return c.barrel; };
        a["getAmmoCount"] = [](AnimationContext& c) { return c.ammo; };
        a["getMaxAmmoCount"] = [](AnimationContext& c) { return c.maxAmmo; };
        a["getMagExtentLevel"] = [](AnimationContext& c) { return c.magExtent; };
        a["hasAmmoToConsume"] = [](AnimationContext& c) { return c.ammoToConsume; };
        a["getFireMode"] = [](AnimationContext& c) { return c.fireMode; };
        a["getShootInterval"] = [](AnimationContext& c) { return c.shootInterval; };
        a["getShootCoolDown"] = [](AnimationContext& c) { return c.shootCooldown; };
        a["getLastShootTimestamp"] = [](AnimationContext& c) { return c.lastShootTimestamp; };
        a["getCurrentTimestamp"] = [](AnimationContext& c) { return c.timestamp; };
        a["adjustClientShootInterval"] = [](AnimationContext& c, double alpha) {
            auto delta = longInteger(alpha);
            if ((delta > 0 && c.shootCooldown > std::numeric_limits<int64_t>::max() - delta)
                || (delta < 0 && c.shootCooldown < std::numeric_limits<int64_t>::min() - delta)) {
                throw std::overflow_error("animation shoot cooldown overflow");
            }
            if (c.adjustShootInterval) c.adjustShootInterval(delta);
            c.shootCooldown = std::max<int64_t>(c.shootCooldown + delta, 0);
        };
        a["getReloadStateType"] = [](AnimationContext& c) { return c.reloadState; };
        a["getPutAwayTime"] = [](AnimationContext& c) { return c.putAwayTime; };
        a["getPartialTicks"] = [](AnimationContext& c) { return c.partialTicks; };
        a["isInputUp"] = [](AnimationContext& c) { return c.inputUp; };
        a["isInputDown"] = [](AnimationContext& c) { return c.inputDown; };
        a["isInputLeft"] = [](AnimationContext& c) { return c.inputLeft; };
        a["isInputRight"] = [](AnimationContext& c) { return c.inputRight; };
        a["isInputJumping"] = [](AnimationContext& c) { return c.inputJumping; };
        a["isOnGround"] = [](AnimationContext& c) { return c.onGround; };
        a["isCrouching"] = [](AnimationContext& c) { return c.crouching; };
        a["isSprinting"] = [](AnimationContext& c) { return c.sprinting; };
        a["anchorWalkDist"] = [](AnimationContext& c) { c.walkAnchor = c.walkDist; };
        a["getWalkDist"] = [](AnimationContext& c) { return c.walkDist - c.walkAnchor; };
        a["popShellFrom"] = [](AnimationContext& c, double index) { if (c.shells.size() < 4096) c.shells.push_back(integer(index)); };
        a["getStateMachineParams"] = [this](AnimationContext&) { return params; };
        a["getNbtAccessor"] = [](AnimationContext& c) { return NbtView{&c, {}}; };
        a["getAttachment"] = [](AnimationContext&, std::string const&) { return "tacz:empty"; };
        a["isAiming"] = [](AnimationContext& c) { return c.aiming; };
        a["getAimingProgress"] = [](AnimationContext& c) { return c.aimingProgress; };
        for (auto key : {"isCrawl", "shouldSlide", "isOverHeat", "isCharging"}) a[key] = [](AnimationContext&) { return false; };
        for (auto key : {"getHeatProgress", "getChargeProgress", "getMaxCharge", "getChargeThreshold"}) a[key] = [](AnimationContext&) { return 0.f; };

        auto n = lua.new_usertype<NbtView>("LuaNbtAccessor", sol::no_constructor);
        n["contains"] = [](NbtView& v, std::string const& key, sol::optional<double> type) {
            auto t = v.read(key, 0);
            return t.is_number_integer() && (!type || (integer(*type) == 99 ? t.get<int>() >= 1 && t.get<int>() <= 6 : t.get<int>() == integer(*type)));
        };
        n["getInt"] = [](NbtView& v, std::string const& k) { return static_cast<int>(longInteger(v.number(k))); };
        n["getLong"] = [](NbtView& v, std::string const& k) { return longInteger(v.number(k)); };
        n["getDouble"] = [](NbtView& v, std::string const& k) { return v.number(k); };
        n["getFloat"] = [](NbtView& v, std::string const& k) { return static_cast<float>(v.number(k)); };
        n["getString"] = [](NbtView& v, std::string const& k) { auto value = v.read(k, 1); return value.is_string() ? value.get<std::string>() : std::string{}; };
        n["getBoolean"] = [](NbtView& v, std::string const& k) { return v.number(k) != 0; };
        n["getCompound"] = [](NbtView& v, std::string const& k, sol::this_state s) -> sol::object {
            if (v.read(k, 0) != 10) return sol::make_object(s, sol::lua_nil);
            auto copy = v;
            copy.path.push_back(k);
            return sol::make_object(s, std::move(copy));
        };
    }
};

std::string_view movementInput(AnimationContext const& c) {
    if (!c.inputAvailable || c.movingSlowly) return "idle";
    if (c.sprinting) return "run";
    return c.moveMagnitude > .01f ? "walk" : "idle";
}

AnimationStateMachine::AnimationStateMachine(pack::GunPackRegistry const& p, animation::AnimationFile const& a,
    pack::ResourceId id, AnimationContext& c, animation::LogFn warn, animation::Controller::SoundFn sounds)
: mImpl(std::make_unique<Impl>(p, a, std::move(id), c, std::move(warn), std::move(sounds))) {}
AnimationStateMachine::~AnimationStateMachine() = default;
bool AnimationStateMachine::initialize(double now) {
    setTime(now);
    return mImpl->protect([&] {
        auto& m = *mImpl;
        if (m.initialized) throw std::runtime_error("state machine already initialized");
        m.globals();
        sol::protected_function require = m.lua["require"];
        auto result = require(m.scriptId.ns + "_" + m.scriptId.path);
        if (!result.valid()) { sol::error error = result; throw std::runtime_error(error.what()); }
        if (result.get_type() != sol::type::table) throw std::runtime_error("state machine must return a table");
        m.script = result.get<sol::table>();
        m.call(m.script, "initialize", std::ref(m.context));
        m.initialized = true;
        auto initial = m.call(m.script, "states");
        if (initial == sol::lua_nil) return;
        if (initial.get_type() != sol::type::table) throw std::runtime_error("states must return a table");
        auto table = initial.as<sol::table>();
        size_t size = table.size();
        if (size > 4096) throw std::runtime_error("animation state limit (4096)");
        m.states.reserve(size);
        for (size_t i = 1; i <= size; ++i) {
            sol::object state = table[i];
            if (state.get_type() != sol::type::table) throw std::runtime_error("state must be a table");
            auto s = m.captureState(state.as<sol::table>());
            m.states.push_back(s);
            m.call(s, "entry", std::ref(m.context));
        }
    });
}
bool AnimationStateMachine::trigger(std::string const& input) { return mImpl->protect([&] { mImpl->trigger(input); }); }
void AnimationStateMachine::setTime(double now) { mImpl->now = now; mImpl->controller.setTime(now); mImpl->debugController.setTime(now); }
animation::Pose AnimationStateMachine::update(double now) {
    setTime(now);
    mImpl->protect([&] {
        if (mImpl->initialized) {
            for (size_t i = 0; i < mImpl->states.size(); ++i) {
                auto state = mImpl->states[i];
                mImpl->call(state, "update", std::ref(mImpl->context));
            }
        }
    });
    auto pose = mImpl->controller.update(now);
    if (mImpl->debugActive) {
        pose = mImpl->debugController.update(now);
        if (mImpl->debugController.info(0).stopped) mImpl->debugActive = false;
    }
    return pose;
}
void AnimationStateMachine::exit() {
    mImpl->protect([&] {
        if (!mImpl->initialized) return;
        mImpl->call(mImpl->script, "exit", std::ref(mImpl->context));
        for (auto const& s : mImpl->states) mImpl->call(s, "exit", std::ref(mImpl->context));
        mImpl->states.clear();
        mImpl->initialized = false;
    });
}
bool AnimationStateMachine::healthy() const { return !mImpl->failed; }
animation::Controller& AnimationStateMachine::controller() { return mImpl->controller; }
bool AnimationStateMachine::playDebug(std::string const& name, double now) {
    if (!mImpl->controller.hasAnimationPrototype(name)) return false;
    mImpl->debugController.clear();
    mImpl->debugController.setTime(now);
    mImpl->debugController.addTrackLine();
    auto track = mImpl->debugController.assignNewTrack(0);
    mImpl->debugController.runAnimation(name, track, false, animation::PlayType::Stop, 0);
    mImpl->debugActive = true;
    return true;
}

} // namespace tacz::lua
