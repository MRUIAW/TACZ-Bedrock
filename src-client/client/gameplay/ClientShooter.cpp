#include "client/gameplay/ClientShooter.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/input/HIDController.h"
#include "mc/deps/input/MouseAction.h"

#include "client/sound/GunSound.h"
#include "gun/ShootRules.h"
#include "mod/TaczMod.h"
#include "nbt/GunItem.h"
#include "net/GunPackets.h"

namespace tacz::client::shooter {
namespace {

using Clock = std::chrono::steady_clock;

int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

// Written from the window procedure / event threads, consumed in frame().
std::atomic<bool> triggerDown{false};
std::atomic<bool> resetPending{false};

// LocalPlayerDataHolder, reduced to what is implemented. Main thread only (frame()).
std::optional<int64_t> lastShot;                 // clientShootTimestamp
bool                   lastShotSucceeded = false; // ShootKey.lastTimeShootSuccess
bool                   dryFireArmed      = true;  // SoundPlayManager.DRY_SOUND_TRACK

// Follow-up shoot sounds of a burst, played when due (the client thread executor only runs
// once per 50 ms tick, too coarse for a 66 ms burst interval).
struct PendingSound {
    Clock::time_point due;
    pack::ResourceId  gunId;
};
std::vector<PendingSound> pendingSounds;

std::mt19937& rng() {
    static std::mt19937 engine{std::random_device{}()};
    return engine;
}

float soundPitch() { return 0.9f + std::uniform_real_distribution<float>(0.0f, 0.125f)(rng()); }

struct HeldGun {
    nbt::GunItemState     state;
    pack::GunEntry const* entry;
};

std::optional<HeldGun> heldGun() {
    auto client = ll::service::getClientInstance();
    if (!client) return std::nullopt;
    auto* player = client->getLocalPlayer();
    if (!player) return std::nullopt;
    auto state = nbt::readGun(player->getSelectedItem());
    if (!state) return std::nullopt;
    auto const* entry = TaczMod::getInstance().gunPacks().gun(state->gunId);
    if (!entry) return std::nullopt;
    return HeldGun{*state, entry};
}

bool inGameplay() {
    auto client = ll::service::getClientInstance();
    return client && client->getLocalPlayer() && client->getScreenName().starts_with("hud_screen");
}

// LocalPlayerShoot.shoot + doShoot. In continuous fire the shot is stamped one interval after
// the previous one instead of "now", so frame timing jitter does not change the fire rate: a
// frame that comes late is made up by the next one (TACZ gets the same effect by scheduling
// the shot `coolDown` ms after its tick on a timer thread).
bool shoot(HeldGun const& gun, int64_t now, bool chained) {
    auto const& data = gun.entry->data;
    auto        mode = gun.state.fireMode;
    if (gun::clientCooldownMs(data, mode, now, lastShot) > 0) return false;

    gun::GunAmmo ammo{gun.state.ammo, gun.state.barrel};
    int          rounds = gun::availableRounds(*data.bolt, ammo);
    if (rounds < 1) {
        if (dryFireArmed) {
            sound::playGunSound(gun.state.gunId, "dry_fire", 1.0f, soundPitch());
            dryFireArmed = false;
        }
        return false;
    }
    if (*data.bolt == pack::Bolt::ManualAction && !ammo.barrel) return false; // bolting comes later

    int64_t interval = gun::triggerIntervalMs(data, mode);
    // A stall longer than an interval (loading, alt-tab) restarts the rhythm instead of
    // firing a catch-up double shot.
    bool    keepRhythm = chained && lastShot && now - *lastShot < interval * 2;
    int64_t shotAt     = keepRhythm ? *lastShot + interval : now;
    lastShot           = shotAt;
    net::ShootPacket{shotAt}.sendToServer();

    // Burst: one packet, `count` rounds; the client only plays the sounds.
    int burst = mode == pack::FireMode::Burst ? std::min(std::max(data.burst.count, 1), rounds) : 1;
    sound::playGunSound(gun.state.gunId, "shoot", 0.8f, soundPitch());
    auto period = std::chrono::milliseconds(gun::burstShootIntervalMs(data));
    for (int i = 1; i < burst; ++i) pendingSounds.push_back({Clock::now() + period * i, gun.state.gunId});
    return true;
}

// Logs once per world how often the frame hook runs, to confirm it is per frame and not per tick.
void measureFrameRate() {
    static std::optional<Clock::time_point> start;
    static int                              frames = 0;
    static bool                             logged = false;
    if (logged) return;
    if (!start) start = Clock::now();
    ++frames;
    auto elapsed = std::chrono::duration<double>(Clock::now() - *start).count();
    if (elapsed < 3.0) return;
    TaczMod::getInstance().getSelf().getLogger().info("[shoot] frame hook rate: {:.0f} Hz", frames / elapsed);
    logged = true;
}

// ShootKey.autoShoot. Runs every rendered frame on the main thread.
void frame() {
    if (resetPending.exchange(false)) {
        lastShot.reset();
        lastShotSucceeded = false;
        dryFireArmed      = true;
        pendingSounds.clear();
    }
    if (inGameplay()) measureFrameRate();
    auto clockNow = Clock::now();
    for (size_t i = 0; i < pendingSounds.size();) {
        if (pendingSounds[i].due > clockNow) {
            ++i;
            continue;
        }
        sound::playGunSound(pendingSounds[i].gunId, "shoot", 0.8f, soundPitch());
        pendingSounds.erase(pendingSounds.begin() + static_cast<std::ptrdiff_t>(i));
    }

    if (!inGameplay()) return;
    auto gun = heldGun();
    if (!gun) {
        lastShotSucceeded = false;
        return;
    }
    if (!triggerDown) {
        lastShotSucceeded = false;
        dryFireArmed      = true;
        return;
    }
    bool continuous = gun::firesContinuously(gun->entry->data, gun->state.fireMode);
    if (!continuous && lastShotSucceeded) return;
    if (shoot(*gun, nowMs(), continuous && lastShotSucceeded)) lastShotSucceeded = true;
}

void fireSelect() {
    auto gun = heldGun();
    if (!gun) return;
    sound::playGunSound(gun->state.gunId, "fire_select", 1.0f, 1.0f);
    net::FireSelectPacket{}.sendToServer();
}

// MinecraftGame::update is the client's per-frame update on the main thread (Lamium drains its
// client tasks here too), so packets and sounds are safe and timing is frame-accurate.
LL_TYPE_INSTANCE_HOOK(ShooterFrameHook, ll::memory::HookPriority::Normal, MinecraftGame, &MinecraftGame::$update, void) {
    origin();
    try {
        frame();
    } catch (std::exception const& e) {
        TaczMod::getInstance().getSelf().getLogger().error("Shooter frame failed: {}", e.what());
    } catch (...) {
        TaczMod::getInstance().getSelf().getLogger().error("Shooter frame failed");
    }
}

bool                                  hooked = false;
std::array<ll::event::ListenerPtr, 3> listeners;

} // namespace

void enable() {
    auto& bus = ll::event::EventBus::getInstance();

    listeners[0] = bus.emplaceListener<ll::event::input::MouseInputEvent>([](ll::event::input::MouseInputEvent& event) {
        if (event.actionButtonId() != MouseAction::ActionLeft) return;
        bool down = event.buttonData() == MouseAction::DataDown;
        if (!down) triggerDown = false; // always release, even if the gun was put away meanwhile
        if (!inGameplay() || !heldGun()) return;
        // Both edges are swallowed while a gun is held (verified in 0.1.x: no mining, no attack).
        event.cancel();
        if (down) triggerDown = true;
    });

    // TACZ's default fire-select key is G.
    listeners[1] = bus.emplaceListener<ll::event::input::KeyInputEvent>([](ll::event::input::KeyInputEvent& event) {
        if (!event.isDown() || event.keyCode() != 'G' || event.controller().mTextboxIsFocused) return;
        if (!inGameplay() || !heldGun()) return;
        event.cancel();
        ll::thread::ClientThreadExecutor::getDefault().execute([] { fireSelect(); });
    });

    listeners[2] = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](ll::event::ClientExitLevelEvent&) {
        triggerDown  = false;
        resetPending = true;
    });

    hooked = ShooterFrameHook::hook() == 0;
    if (!hooked) TaczMod::getInstance().getSelf().getLogger().warn("Cannot hook MinecraftGame::update; guns cannot fire");
}

void disable() {
    if (hooked) ShooterFrameHook::unhook();
    hooked = false;
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : listeners) {
        if (listener) bus.removeListener(listener);
        listener = nullptr;
    }
    triggerDown  = false;
    resetPending = true;
}

} // namespace tacz::client::shooter
