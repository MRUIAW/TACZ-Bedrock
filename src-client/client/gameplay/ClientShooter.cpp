#include "client/gameplay/ClientShooter.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/MinecraftGame.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/input/HIDController.h"
#include "mc/deps/input/MouseAction.h"

#include "client/sound/GunSound.h"
#include "gun/ShootRules.h"
#include "mod/TaczMod.h"
#include "nbt/AmmoItem.h"
#include "nbt/GunItem.h"
#include "net/GunPackets.h"
#include "pack/AnimationSounds.h"
#include "pack/GunPackParser.h"

namespace tacz::client::shooter {
namespace {

using Clock = std::chrono::steady_clock;

int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

// Written from the window procedure / packet threads, consumed in frame().
std::atomic<bool> triggerDown{false};
std::atomic<bool> resetPending{false};
std::atomic<bool> reloadKeyPressed{false};
std::atomic<bool> fireSelectPressed{false};

// Server-reported reload state (TACZ's synced RELOAD_STATE_KEY). Written by the packet handler.
std::mutex                          serverStateMutex;
std::optional<gun::ReloadState>     serverReloadState;

// LocalPlayerDataHolder, reduced to what is implemented. Main thread only (frame()).
std::optional<int64_t> lastShot;                 // clientShootTimestamp
bool                   lastShotSucceeded = false; // ShootKey.lastTimeShootSuccess
bool                   dryFireArmed      = true;  // SoundPlayManager.DRY_SOUND_TRACK
gun::ReloadStateType   reloadType        = gun::ReloadStateType::NotReloading; // getSynReloadState()
std::atomic<int>       reloadTypePublished{0};
// Client state lock (LocalPlayerDataHolder.clientStateLock): set when a reload is requested,
// held until the server confirms or refuses it, or 250 ms pass.
std::optional<Clock::time_point> reloadLockedAt;

// Sounds to play at a given time: burst follow-up shots, reload animation keyframes.
struct PendingSound {
    Clock::time_point due;
    pack::ResourceId  sound;
    float             volume;
    float             pitch;
    bool              reload; // cut off by stopReloadSounds (SoundPlayManager.stopPlayGunSound)
};
std::vector<PendingSound> pendingSounds;

// Animation sound keyframes per animation file, parsed on first use.
std::map<pack::ResourceId, std::map<std::string, std::vector<pack::AnimationSound>>> animationSounds;

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

std::optional<pack::ResourceId> displaySound(HeldGun const& gun, std::string const& key) {
    auto const& packs = TaczMod::getInstance().gunPacks();
    if (!gun.entry->index.display) return std::nullopt;
    auto const* display = packs.gunDisplay(*gun.entry->index.display);
    if (!display) return std::nullopt;
    auto it = display->sounds.find(key);
    return it == display->sounds.end() ? std::nullopt : std::optional{it->second};
}

std::vector<pack::AnimationSound> const* animationKeyframes(HeldGun const& gun, std::string const& animation) {
    auto const& packs = TaczMod::getInstance().gunPacks();
    if (!gun.entry->index.display) return nullptr;
    auto const* display = packs.gunDisplay(*gun.entry->index.display);
    if (!display || !display->animation) return nullptr;
    auto it = animationSounds.find(*display->animation);
    if (it == animationSounds.end()) {
        std::map<std::string, std::vector<pack::AnimationSound>> parsed;
        if (auto text = packs.readFile(pack::assetPath(*display->animation, "animations", ".animation.json"))) {
            try {
                parsed = pack::parseAnimationSounds(pack::parseJsonText(*text));
            } catch (std::exception const& e) {
                logger().warn("Animation {} cannot be read: {}", display->animation->toString(), e.what());
            }
        }
        it = animationSounds.emplace(*display->animation, std::move(parsed)).first;
    }
    auto found = it->second.find(animation);
    return found == it->second.end() ? nullptr : &found->second;
}

void stopReloadSounds() {
    std::erase_if(pendingSounds, [](PendingSound const& s) { return s.reload; });
}

// LocalPlayerReload.doReload. TACZ plays the display's reload sound and the animation state
// machine plays the reload animation, whose sound keyframes carry the actual reload sounds.
// The animation system is not ported yet, so the keyframes of the animation the default and
// AK47 state machines would pick (reload_empty / reload_tactical, no extended magazine) are
// scheduled directly.
void playReloadSounds(HeldGun const& gun, bool noAmmo) {
    stopReloadSounds();
    if (auto id = displaySound(gun, noAmmo ? "reload_empty" : "reload_tactical")) sound::play(*id, 1.0f, 1.0f);
    auto const* keyframes = animationKeyframes(gun, noAmmo ? "reload_empty" : "reload_tactical");
    if (!keyframes) return;
    auto start = Clock::now();
    for (auto const& k : *keyframes) {
        auto due = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(k.time));
        pendingSounds.push_back({due, k.sound, 1.0f, 1.0f, true});
    }
}

// LocalPlayerReload.reload.
void reload(HeldGun const& gun) {
    auto const& data = gun.entry->data;
    if (data.reload.type == pack::FeedType::Inventory) return;
    if (reloadLockedAt || gun::isReloading(reloadType)) return;
    if (lastShot && nowMs() - *lastShot < 100) return;
    // canReload: not full and, outside creative, carrying the ammo.
    auto client = ll::service::getClientInstance();
    auto* player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return;
    if (gun.state.ammo >= data.ammoAmount) return;
    bool creative = player->isCreative();
    if (!creative && !data.reload.infinite && !(data.ammoId && nbt::countAmmo(*player, *data.ammoId) > 0)) return;

    reloadLockedAt = Clock::now();
    net::ReloadPacket{}.sendToServer();
    bool noAmmo = data.bolt == pack::Bolt::OpenBolt ? gun.state.ammo <= 0 : !gun.state.barrel;
    playReloadSounds(gun, noAmmo);
}

// LocalPlayerShoot.shoot + doShoot. In continuous fire the shot is stamped one interval after
// the previous one instead of "now", so frame timing jitter does not change the fire rate: a
// frame that comes late is made up by the next one (TACZ gets the same effect by scheduling
// the shot `coolDown` ms after its tick on a timer thread).
bool shoot(HeldGun const& gun, int64_t now, bool chained) {
    auto const& data = gun.entry->data;
    auto        mode = gun.state.fireMode;
    if (gun::clientCooldownMs(data, mode, now, lastShot) > 0) return false;
    // preCheck: no shooting while reloading.
    if (gun::isReloading(reloadType) || reloadLockedAt) return false;

    gun::GunAmmo ammo{gun.state.ammo, gun.state.barrel};
    int          rounds = gun::availableRounds(*data.bolt, ammo);
    if (rounds < 1) {
        if (dryFireArmed) {
            if (auto id = displaySound(gun, "dry_fire")) sound::play(*id, 1.0f, soundPitch());
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
    auto shootSound = displaySound(gun, "shoot");
    if (!shootSound) return true;
    sound::play(*shootSound, 0.8f, soundPitch());
    int  burst  = mode == pack::FireMode::Burst ? std::min(std::max(data.burst.count, 1), rounds) : 1;
    auto period = std::chrono::milliseconds(gun::burstShootIntervalMs(data));
    for (int i = 1; i < burst; ++i) pendingSounds.push_back({Clock::now() + period * i, *shootSound, 0.8f, soundPitch(), false});
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
    logger().info("[shoot] frame hook rate: {:.0f} Hz", frames / elapsed);
    logged = true;
}

void applyServerReloadState() {
    std::optional<gun::ReloadState> state;
    {
        std::lock_guard lock(serverStateMutex);
        state.swap(serverReloadState);
    }
    if (!state) return;
    reloadLockedAt.reset();
    // A refused or stopped reload cuts its sounds (stopPlayGunSound on the next action in TACZ).
    if (!gun::isReloading(state->type) && gun::isReloading(reloadType) && !gun::isReloadFinishing(reloadType)) {
        stopReloadSounds();
    }
    if (!gun::isReloading(state->type) && !gun::isReloading(reloadType)) stopReloadSounds();
    reloadType = state->type;
    reloadTypePublished = static_cast<int>(reloadType);
}

// ShootKey.autoShoot and the reload / fire-select keys. Runs every rendered frame on the main thread.
void frame() {
    if (resetPending.exchange(false)) {
        lastShot.reset();
        lastShotSucceeded = false;
        dryFireArmed      = true;
        reloadType        = gun::ReloadStateType::NotReloading;
        reloadTypePublished = 0;
        reloadLockedAt.reset();
        pendingSounds.clear();
        animationSounds.clear();
    }
    applyServerReloadState();
    // tickStateLock: a request the server never answered stops blocking after 250 ms.
    if (reloadLockedAt && Clock::now() - *reloadLockedAt > std::chrono::milliseconds(250) && !gun::isReloading(reloadType)) {
        reloadLockedAt.reset();
    }
    if (inGameplay()) measureFrameRate();

    auto clockNow = Clock::now();
    for (size_t i = 0; i < pendingSounds.size();) {
        if (pendingSounds[i].due > clockNow) {
            ++i;
            continue;
        }
        sound::play(pendingSounds[i].sound, pendingSounds[i].volume, pendingSounds[i].pitch);
        pendingSounds.erase(pendingSounds.begin() + static_cast<std::ptrdiff_t>(i));
    }

    bool wantsReload     = reloadKeyPressed.exchange(false);
    bool wantsFireSelect = fireSelectPressed.exchange(false);
    if (!inGameplay()) return;
    auto gun = heldGun();
    if (!gun) {
        lastShotSucceeded = false;
        return;
    }
    if (wantsFireSelect && !gun::isReloading(reloadType)) {
        if (auto id = displaySound(*gun, "fire_select")) sound::play(*id, 1.0f, 1.0f);
        net::FireSelectPacket{}.sendToServer();
    }
    if (wantsReload) reload(*gun);

    if (!triggerDown) {
        lastShotSucceeded = false;
        dryFireArmed      = true;
        return;
    }
    // LocalPlayerMixin.shoot: pulling the trigger interrupts a reload first.
    if (gun::isReloading(reloadType)) {
        if (!lastShotSucceeded) net::CancelReloadPacket{}.sendToServer();
        lastShotSucceeded = true; // one cancel per press
        return;
    }
    bool continuous = gun::firesContinuously(gun->entry->data, gun->state.fireMode);
    if (!continuous && lastShotSucceeded) return;
    if (shoot(*gun, nowMs(), continuous && lastShotSucceeded)) lastShotSucceeded = true;
}

class ReloadStateHandler final : public ll::network::PacketHandlerBase<ReloadStateHandler, net::ReloadStatePacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, net::ReloadStatePacket const& packet) const {
        auto type = gun::reloadStateFromOrdinal(packet.state);
        if (!type) return;
        std::lock_guard lock(serverStateMutex);
        serverReloadState = gun::ReloadState{*type, packet.countDown};
    }
};

// MinecraftGame::update is the client's per-frame update on the main thread (Lamium drains its
// client tasks here too), so packets and sounds are safe and timing is frame-accurate.
LL_TYPE_INSTANCE_HOOK(ShooterFrameHook, ll::memory::HookPriority::Normal, MinecraftGame, &MinecraftGame::$update, void) {
    origin();
    try {
        frame();
    } catch (std::exception const& e) {
        logger().error("Shooter frame failed: {}", e.what());
    } catch (...) {
        logger().error("Shooter frame failed");
    }
}

bool                                  hooked = false;
std::array<ll::event::ListenerPtr, 3> listeners;

} // namespace

gun::ReloadStateType reloadState() { return static_cast<gun::ReloadStateType>(reloadTypePublished.load()); }

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

    // TACZ's default keys: G fire select, R reload.
    listeners[1] = bus.emplaceListener<ll::event::input::KeyInputEvent>([](ll::event::input::KeyInputEvent& event) {
        if (!event.isDown() || event.controller().mTextboxIsFocused) return;
        int key = event.keyCode();
        if (key != 'G' && key != 'R') return;
        if (!inGameplay() || !heldGun()) return;
        event.cancel();
        (key == 'G' ? fireSelectPressed : reloadKeyPressed) = true;
    });

    listeners[2] = bus.emplaceListener<ll::event::ClientExitLevelEvent>([](ll::event::ClientExitLevelEvent&) {
        triggerDown  = false;
        resetPending = true;
    });

    hooked = ShooterFrameHook::hook() == 0;
    if (!hooked) logger().warn("Cannot hook MinecraftGame::update; guns cannot fire");
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
