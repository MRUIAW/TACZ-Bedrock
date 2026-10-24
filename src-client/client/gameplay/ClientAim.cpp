#include "client/gameplay/ClientAim.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <type_traits>

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/deps/core/math/Vec2.h"
#include "client/gameplay/ClientShooter.h"
#include "model/Aiming.h"
#include "mod/TaczMod.h"
#include "net/GunPackets.h"

namespace tacz::client::aim {
namespace {
using Clock = std::chrono::steady_clock;
using FovSignature = float (LevelRendererPlayer::*)(float, bool) const;
using TurnSignature = void (LocalPlayer::*)(Vec2 const&);
using FocusSignature = void (MinecraftGame::*)();
static_assert(std::is_same_v<decltype(&LevelRendererPlayer::getFov), FovSignature>);
static_assert(std::is_same_v<decltype(&LocalPlayer::_applyTurnDelta), TurnSignature>);
static_assert(std::is_same_v<decltype(&MinecraftGame::$onAppFocusLost), FocusSignature>);
static_assert(std::is_same_v<decltype(&LocalPlayer::$setSprinting), void (LocalPlayer::*)(bool)>);

std::atomic<bool> rightDown{false}, swallowed{false}, focusReset{false};
std::atomic<unsigned> togglePresses{0};
bool holdToAim = true; // read from our own config at enable, before callbacks install
bool toggled = false, target = false, tickValid = false;
bool loggedFull = false;
float previous = 0, current = 0, adsSeconds = .2f;
std::optional<Clock::time_point> lastTick, lastFrame;
model::AimDynamics view;
std::mutex publishMutex;
Snapshot published;

std::mutex cameraMutex;
uint64_t cameraWorld = 0;
model::AimDynamics worldFov(.5f, 1.2f, .5f);
std::optional<Clock::time_point> fovAt;
float lastBaseFov = 70;
bool fovHooked = false, turnHooked = false, focusHooked = false;
bool loggedFov = false;

void setTarget(bool value) {
    if (value == target) return;
    target = value;
    if (!value) loggedFull = false;
    net::AimPacket{value}.sendToServer();
    TaczMod::getInstance().getSelf().getLogger().info("[ads] world={} aiming={}", shooter::worldToken(), value);
}

// Same typed hook point used by Lamium CameraSessions; compose with origin rather than
// overwrite the user's FOV option. No lookup of primary client/player/level in this hook.
LL_TYPE_INSTANCE_HOOK(AimFovHook, ll::memory::HookPriority::Normal, LevelRendererPlayer,
    &LevelRendererPlayer::getFov, float, float alpha, bool variable) {
    auto base = origin(alpha, variable);
    if (!shooter::worldToken() || !variable) return base;
    auto state = snapshot();
    if (!state.world || !state.gun || state.client != reinterpret_cast<uintptr_t>(&mClientInstance)) return base;
    if (!std::isfinite(base) || base <= 0 || base >= 180) return base;
    std::lock_guard lock(cameraMutex);
    auto now = Clock::now();
    if (cameraWorld != state.world || !fovAt || now - *fovAt > std::chrono::seconds(1)) {
        worldFov.reset(base);
        cameraWorld = state.world;
        fovAt = now;
        loggedFov = false;
    }
    lastBaseFov = base;
    auto desired = model::aimingFov(base, state.ironZoom, state.progress);
    auto value = worldFov.update(desired, std::chrono::duration<double>(now - *fovAt).count());
    fovAt = now;
    if (state.progress <= 0) loggedFov = false;
    if (state.progress >= 1 && !loggedFov) {
        TaczMod::getInstance().getSelf().getLogger().info(
            "[ads-fov] world={} base={:.2f} ironZoom={:.3f} target={:.2f} smoothed={:.2f} variable={}",
            state.world, base, state.ironZoom, desired, value, variable);
        loggedFov = true;
    }
    return std::clamp(value, 1.f, 179.f);
}
LL_TYPE_INSTANCE_HOOK(AimSensitivityHook, ll::memory::HookPriority::Normal, LocalPlayer,
    &LocalPlayer::_applyTurnDelta, void, Vec2 const& delta) {
    if (!shooter::worldToken()) { origin(delta); return; }
    auto state = snapshot();
    if (!state.world || !state.gun || state.player != reinterpret_cast<uintptr_t>(this)) { origin(delta); return; }
    float base;
    { std::lock_guard lock(cameraMutex); base = lastBaseFov; }
    auto scale = model::aimingSensitivity(base, state.ironZoom, state.progress);
    Vec2 applied{delta.x * scale, delta.z * scale};
    origin(applied);
}
LL_TYPE_INSTANCE_HOOK(AimFocusHook, ll::memory::HookPriority::Normal, MinecraftGame,
    &MinecraftGame::$onAppFocusLost, void) {
    rightDown = false;
    togglePresses = 0;
    focusReset = true; // do not send gameplay packets or mutate frame-owned state here
    origin();
}
}

bool mouse(bool down, bool gameplay, bool gunHeld) {
    bool was = rightDown.exchange(down && gameplay);
    bool release = !down && swallowed.exchange(false);
    if (!gameplay || !gunHeld) return release;
    if (down) {
        swallowed = true;
        if (!was) ++togglePresses;
    }
    return true;
}
void frame(IClientInstance& client, LocalPlayer& player, std::optional<pack::ResourceId> gun,
    pack::GunData const* data, pack::GunDisplay const* display, bool selectionChanged, bool allowed) {
    auto now = Clock::now();
    double elapsed = lastFrame ? std::chrono::duration<double>(now - *lastFrame).count() : 0;
    lastFrame = now;
    bool gameplay = client.getScreenName().starts_with("hud_screen") && player.isAlive() && !player.isSpectator();
    bool reset = focusReset.exchange(false);
    if (selectionChanged || reset) {
        setTarget(false);
        previous = current = 0;
        view.reset();
        lastTick = now;
        toggled = false;
    }
    auto presses = togglePresses.exchange(0);
    if (!gameplay || reset) { rightDown = false; toggled = false; }
    else if (presses % 2) toggled = !toggled;
    if (!allowed) toggled = false; // unlike hold pre-input, toggle cannot persist through put-away
    bool usable = gameplay && !reset && gun && data && display && allowed;
    setTarget(usable && (holdToAim ? rightDown.load() : toggled));
    tickValid = usable;
    adsSeconds = data ? model::effectiveAdsTime(data->aimTime) : .2f; // unmodified ads cache; attachments deferred
    if (!usable) {
        previous = current = 0;
        view.reset();
        std::lock_guard lock(cameraMutex);
        cameraWorld = 0;
        fovAt.reset(); // UI/put-away must not resurrect a previous zoom on re-entry
    }
    if (target) player.setSprinting(false);
    float progress = std::lerp(previous, current, std::clamp(client.getFrameAlpha(), 0.f, 1.f));
    float visual = view.update(progress, elapsed);
    if (progress >= 1 && !loggedFull && gun) {
        TaczMod::getInstance().getSelf().getLogger().info(
            "[ads] world={} gun={} full progress=1 adsSeconds={:.3f} ironZoom={:.3f} modelFov={:.2f}",
            shooter::worldToken(), gun->toString(), adsSeconds, display ? display->ironZoom : 1,
            display ? display->zoomModelFov : 70);
        loggedFull = true;
    }
    Snapshot next;
    if (gameplay && gun && data && display && allowed) {
        next = {shooter::worldToken(), reinterpret_cast<uintptr_t>(&client), reinterpret_cast<uintptr_t>(&player),
            gun, target, progress, visual, display->ironZoom, display->zoomModelFov};
    }
    std::lock_guard lock(publishMutex);
    published = std::move(next);
}
void tick() {
    if (!shooter::worldToken()) return;
    auto now = Clock::now();
    double elapsed = lastTick ? std::chrono::duration<double, std::milli>(now - *lastTick).count() : 50;
    lastTick = now;
    previous = current;
    current = tickValid ? model::advanceAimProgress(current, target, elapsed, adsSeconds) : 0;
}
Snapshot snapshot() {
    auto world = shooter::worldToken();
    if (!world) return {};
    std::lock_guard lock(publishMutex);
    return published.world == world ? published : Snapshot{};
}
void clear() {
    rightDown = swallowed = false;
    togglePresses = 0;
    focusReset = false;
    target = toggled = tickValid = false;
    loggedFull = false;
    previous = current = 0;
    lastTick.reset(); lastFrame.reset();
    view.reset();
    { std::lock_guard lock(publishMutex); published = {}; }
    { std::lock_guard lock(cameraMutex); cameraWorld = 0; fovAt.reset(); lastBaseFov = 70; }
}
void enable() {
    clear();
    holdToAim = true;
    auto& mod = TaczMod::getInstance();
    try {
        auto path = mod.getSelf().getModDir() / "config" / "aim.json";
        if (std::ifstream input{path}; input) {
            auto config = nlohmann::json::parse(input);
            holdToAim = config.value("hold_to_aim", true);
        } else {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream output{path};
            output << "{\n  \"hold_to_aim\": true\n}\n";
        }
    } catch (std::exception const& e) {
        holdToAim = true;
        mod.getSelf().getLogger().warn("[ads] config unavailable; hold-to-aim default: {}", e.what());
    }
    fovHooked = AimFovHook::hook() == 0;
    turnHooked = AimSensitivityHook::hook() == 0;
    focusHooked = AimFocusHook::hook() == 0;
    mod.getSelf().getLogger().info("[ads] holdToAim={} worldFovHook={} sensitivityHook={} focusHook={}",
        holdToAim, fovHooked, turnHooked, focusHooked);
}
void disable() {
    if (fovHooked) AimFovHook::unhook();
    if (turnHooked) AimSensitivityHook::unhook();
    if (focusHooked) AimFocusHook::unhook();
    fovHooked = turnHooked = focusHooked = false;
    clear();
}
} // namespace tacz::client::aim
