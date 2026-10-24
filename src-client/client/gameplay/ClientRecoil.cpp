#include "client/gameplay/ClientRecoil.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <random>
#include <set>
#include <type_traits>
#include <glm/vec2.hpp>

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/math/Vec2.h"
#include "client/gameplay/ClientAim.h"
#include "client/gameplay/ClientShooter.h"
#include "model/CameraRecoil.h"
#include "mod/TaczMod.h"

namespace tacz::client::recoil {
namespace {
using Clock = std::chrono::steady_clock;
double nowMs() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }
using TurnSignature = void (LocalPlayer::*)(Vec2 const&);
using FocusSignature = void (MinecraftGame::*)();
static_assert(std::is_same_v<decltype(&LocalPlayer::_applyTurnDelta), TurnSignature>);
static_assert(std::is_same_v<decltype(&MinecraftGame::$onAppFocusLost), FocusSignature>);
static_assert(std::is_same_v<decltype(&MinecraftGame::$onAppFocusGained), FocusSignature>);

model::CameraRecoil camera;
std::atomic<bool> focused{true}, cancellation{false};
uint64_t ownerWorld = 0, statsWorld = 0, shots = 0, steps = 0;
uintptr_t ownerClient = 0, ownerPlayer = 0;
std::optional<pack::ResourceId> gunId;
std::optional<glm::vec2> lastObserved;
std::set<std::string> warned;
bool lostHooked = false, gainedHooked = false;
thread_local LocalPlayer const* programmaticPlayer = nullptr;

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

// No client/player/world queries in app/menu callbacks. Invalidation is consumed on the
// existing game frame thread, and nothing resumes an old recoil tail after focus returns.
LL_TYPE_INSTANCE_HOOK(RecoilFocusLost, ll::memory::HookPriority::Normal, MinecraftGame,
    &MinecraftGame::$onAppFocusLost, void) {
    focused = false;
    cancellation = true;
    origin();
}
LL_TYPE_INSTANCE_HOOK(RecoilFocusGained, ll::memory::HookPriority::Normal, MinecraftGame,
    &MinecraftGame::$onAppFocusGained, void) {
    cancellation = true;
    focused = true;
    origin();
}
}

bool isApplyingTurn(LocalPlayer const& player) { return programmaticPlayer == &player; }
void requestClear() { cancellation = true; }
void clear() {
    camera.clear();
    ownerWorld = 0;
    ownerClient = ownerPlayer = 0;
    gunId.reset();
    lastObserved.reset();
}
void shot(pack::GunRecoil const& data, pack::ResourceId const& gun, int64_t triggerTimestamp, bool burstRound) {
    auto world = shooter::worldToken();
    if (!world || !focused.load()) return;
    if (cancellation.exchange(false)) clear();
    auto aiming = aim::snapshot();
    if (aiming.world != world || aiming.gun != gun) return; // UI/put-away/nonlocal/stale selection
    if (statsWorld != world) { statsWorld = world; shots = steps = 0; }
    // RecoilModifier is neutral without attachments; no crawl pose has been implemented.
    auto modifier = model::cameraRecoilModifier(aiming.progress, aiming.ironZoom, 1, 1);
    static std::mt19937 random{std::random_device{}()};
    auto pitch = model::buildRecoilCurve(data.pitch, modifier, random);
    auto yaw = model::buildRecoilCurve(data.yaw, modifier, random);
    for (auto const& [axis, error] : {std::pair{"pitch", pitch.error}, std::pair{"yaw", yaw.error}}) {
        if (!error.empty() && warned.insert(gun.toString() + ":" + axis + ":" + error).second)
            logger().warn("[recoil] {} {} disabled: {}", gun.toString(), axis, error);
    }
    ++shots;
    if (shots <= 8) logger().info(
        "[recoil-shot] world={} shot={} gun={} triggerTs={} burstRound={} aim={:.3f} ironZoom={:.3f} modifier={:.5f} pitchEndMs={:.1f} yawEndMs={:.1f} pitch30={:.5f} yaw30={:.5f} replacedActive={}",
        world, shots, gun.toString(), triggerTimestamp, burstRound, aiming.progress, aiming.ironZoom, modifier,
        pitch.spline ? pitch.spline->endMs() : 0, yaw.spline ? yaw.spline->endMs() : 0,
        pitch.spline ? pitch.spline->evaluate(30).value_or(0) : 0,
        yaw.spline ? yaw.spline->evaluate(30).value_or(0) : 0, camera.active());
    camera.shot(std::move(pitch.spline), std::move(yaw.spline), nowMs());
    ownerWorld = world;
    ownerClient = aiming.client;
    ownerPlayer = aiming.player;
    gunId = gun;
}
void update(IClientInstance& client, LocalPlayer& player) {
    auto world = shooter::worldToken();
    if (cancellation.exchange(false)) clear();
    if (!world || !focused.load()) { clear(); return; }
    if (!camera.active()) return;
    // The caller has already validated the joined-client identity. Retain no native owner;
    // exact world/client/player and selected gun identities are checked again per frame.
    auto aiming = aim::snapshot();
    if (world != ownerWorld || reinterpret_cast<uintptr_t>(&client) != ownerClient
        || reinterpret_cast<uintptr_t>(&player) != ownerPlayer || aiming.world != world || aiming.gun != gunId
        || !player.isAlive() || player.isSpectator() || !client.getScreenName().starts_with("hud_screen")) {
        clear();
        return;
    }
    auto const& before = player.getRotation();
    float pitchBefore = before.x, yawBefore = before.z;
    if (!std::isfinite(pitchBefore) || !std::isfinite(yawBefore)) { clear(); return; }
    auto delta = camera.update(nowMs());
    if (delta.pitch == 0 && delta.yaw == 0) return;
    if (!std::isfinite(delta.pitch) || !std::isfinite(delta.yaw)) { clear(); return; }
    // This is a real local look turn, not a transient projection/view-matrix camera shake.
    // No copy of an ItemStack and no fabricated movement/PlayerAuthInput packet is needed.
    auto pitchTurn = static_cast<float>(delta.pitch), yawTurn = static_cast<float>(delta.yaw);
    if (!std::isfinite(pitchTurn) || !std::isfinite(yawTurn)) { clear(); return; }
    struct TurnScope {
        LocalPlayer const* previous;
        explicit TurnScope(LocalPlayer const& p) : previous(programmaticPlayer) { programmaticPlayer = &p; }
        ~TurnScope() { programmaticPlayer = previous; }
    };
    {
        TurnScope unscaled{player};
        player._applyTurnDelta(Vec2{pitchTurn, yawTurn});
    }
    auto const& after = player.getRotation();
    if (steps == 0 && after.x == pitchBefore && after.z == yawBefore && std::abs(pitchBefore) < 85
        && std::hypot(delta.pitch, delta.yaw) > .0001)
        logger().warn("[recoil-look] native turn returned without an immediate actor-rotation change; check detached camera/mod input paths in the runtime trace");
    if (++steps <= 12) logger().info(
        "[recoil-look] world={} shot={} frameStep={} requested=({:.6f},{:.6f}) before=({:.6f},{:.6f}) after=({:.6f},{:.6f}) previousObserved=({:.6f},{:.6f}) mdvBypassed=true native-turn=true (degrees; other input/corrections may also change rotation)",
        world, shots, steps, delta.pitch, delta.yaw, pitchBefore, yawBefore, after.x, after.z,
        lastObserved ? lastObserved->x : pitchBefore, lastObserved ? lastObserved->y : yawBefore);
    lastObserved = glm::vec2{after.x, after.z};
}
void enable() {
    clear();
    focused = true;
    cancellation = false;
    warned.clear();
    lostHooked = RecoilFocusLost::hook() == 0;
    gainedHooked = RecoilFocusGained::hook() == 0;
    logger().info("[recoil] native look-turn adapter enabled; focus hooks={}/{}; natural cubic +30ms, replacement overlap, MDV bypass", lostHooked, gainedHooked);
}
void disable() {
    if (lostHooked) RecoilFocusLost::unhook();
    if (gainedHooked) RecoilFocusGained::unhook();
    lostHooked = gainedHooked = false;
    clear();
    logger().info("[recoil] disabled: lastWorld={} shots={} appliedFrames={}", statsWorld, shots, steps);
}
} // namespace tacz::client::recoil
