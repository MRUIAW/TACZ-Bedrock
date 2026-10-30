#include "client/sound/GunSound.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/sound/SoundEngine.h"
#include "mc/deps/core/file/Path.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/platform/UUID.h"

#include "client/gameplay/ClientShooter.h"
#include "mod/TaczMod.h"
#include "pack/GunDisplay.h"

namespace tacz::client::sound {
namespace {

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

std::mutex mutex;
tacz::sound::SoundPlaybackState playback;
tacz::sound::SoundCleanupCadence cleanupCadence;
Bedrock::NonOwnerPointer<SoundEngine> trackedEngine;
uint64_t trackedWorld = 0;
struct Owner { uint64_t runtimeId; mce::UUID uuid; };
std::optional<Owner> trackedOwner;

// Caller holds mutex. Never send old handles to a newly looked-up engine.
void stopAllLocked() {
    playback.clear([](auto handle) { if (auto engine = trackedEngine.get()) engine->stop(handle); });
    trackedEngine = {}; trackedWorld = 0; trackedOwner.reset();
}
void ensureScope(SoundEngine& engine, uint64_t world, LocalPlayer& player) {
    auto runtimeId = player.getRuntimeID().rawID;
    if (trackedWorld == world && trackedEngine.get() == &engine && trackedOwner
        && trackedOwner->runtimeId == runtimeId && trackedOwner->uuid == player.getUuid()) return;
    // A new local player/world/engine cannot inherit the old owner's voices.
    stopAllLocked();
    trackedEngine = Bedrock::NonOwnerPointer<SoundEngine>(engine);
    trackedWorld = world;
    trackedOwner = Owner{runtimeId, player.getUuid()};
    playback.scope(world, reinterpret_cast<uintptr_t>(&engine), [](auto) {});
}

// The engine may stream from these bytes after playFromMemory returns. Keep
// one shared immutable Ogg buffer per logical id alive for the process lifetime.
// A null entry marks a missing file so it is reported only once per cache load.
auto& cache = *new std::map<pack::ResourceId, std::shared_ptr<std::string const>>;

std::shared_ptr<std::string const> load(pack::ResourceId const& id) {
    std::lock_guard lock(mutex);
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    auto bytes = TaczMod::getInstance().gunPacks().readFile(pack::assetPath(id, "tacz_sounds", ".ogg"), pack::ResourceLookup::Listed);
    if (!bytes) logger().info("Sound {} is not in any gun pack; skipped", id.toString());
    auto data = bytes ? std::make_shared<std::string const>(std::move(*bytes)) : nullptr;
    cache.emplace(id, data);
    return data;
}

void cleanupTick(ClientLevel& level) {
    auto client = ll::service::getClientInstance();
    if (!client || !shooter::ownsClient(&*client) || client->getLevel() != &level) return;
    std::lock_guard lock(mutex);
    auto world = shooter::worldToken();
    if (!world) { stopAllLocked(); return; }
    bool cleanupDue = cleanupCadence.tick(); // count real ticks even with no tracked voices
    if (world != trackedWorld) { stopAllLocked(); return; }
    if (!cleanupDue) return;
    auto engine = trackedEngine.get();
    if (!engine) { stopAllLocked(); return; }
    auto player = client->getLocalPlayer();
    bool valid = player && player->isAlive() && !player->isDead() && trackedOwner
        && player->getRuntimeID().rawID == trackedOwner->runtimeId && player->getUuid() == trackedOwner->uuid;
    playback.cleanup([&](auto h) { return engine->isPlayingSound(h); },
        [&](auto owner) { return valid && owner == trackedOwner->runtimeId; }, [&](auto h) { engine->stop(h); });
}

// LL's ClientLevelTickEvent publishes BEFORE origin(). Java uses END, so use
// the same native tick target but run sound cleanup AFTER origin(), not per frame.
LL_TYPE_INSTANCE_HOOK(SoundCleanupTickHook, ll::memory::HookPriority::Normal, ClientLevel,
    &ClientLevel::$_subTick, void) {
    origin();
    cleanupTick(*this);
}
bool hooked = false;

} // namespace

bool play(pack::ResourceId const& soundId, float volume, float pitch, Category category, bool temporaryBase) {
    auto world = shooter::worldToken();
    if (!world) return false;
    auto client = ll::service::getClientInstance();
    if (!client || !shooter::ownsClient(&*client)) return false;
    auto player = client->getLocalPlayer();
    if (!player) return false;
    auto data = load(soundId);
    if (!data) {
        if (temporaryBase) { std::lock_guard lock(mutex); playback.forgetTemporary(); }
        return false;
    }
    auto engine = client->getSoundEngine();
    std::lock_guard lock(mutex);
    ensureScope(*engine, world, *player);
    auto name = "tacz/" + soundId.ns + "/" + soundId.path;
    auto handle = playback.play(trackedOwner->runtimeId, soundId, tacz::sound::concurrencyLimit(category), temporaryBase,
        [&](auto h) { return engine->isPlayingSound(h); }, [&](auto h) { engine->stop(h); }, [&](size_t slot) {
            return engine->playFromMemory(Core::Path(name + "#" + std::to_string(slot)), data->data(), data->size(), false,
                1.0f, 16.0f, volume, pitch, false, false);
        });
    return handle != 0;
}

bool playGunSound(pack::ResourceId const& gunId, std::string const& key, float volume, float pitch, bool temporaryBase) {
    auto missing = [&] {
        if (temporaryBase) { std::lock_guard lock(mutex); playback.forgetTemporary(); }
        return false;
    };
    auto const& packs = TaczMod::getInstance().gunPacks();
    auto const* gun = packs.gun(gunId);
    if (!gun || !gun->index.display) return missing();
    auto const* display = packs.gunDisplay(*gun->index.display);
    if (!display) return missing();
    auto it = display->sounds.find(key);
    if (it == display->sounds.end()) return missing();
    return play(it->second, volume, pitch, tacz::sound::gunSoundCategory(key), temporaryBase);
}

void stopTemporary() {
    std::lock_guard lock(mutex);
    if (trackedWorld && trackedWorld == shooter::worldToken()) if (auto engine = trackedEngine.get()) {
        playback.stopTemporary([&](auto h) { engine->stop(h); });
    }
}
void stopTemporary(pack::ResourceId const& expectedSound) {
    std::lock_guard lock(mutex);
    if (trackedWorld && trackedWorld == shooter::worldToken()) if (auto engine = trackedEngine.get()) {
        playback.stopTemporary([&](auto h) { engine->stop(h); }, expectedSound);
    }
}
void stopAllTracked() { std::lock_guard lock(mutex); stopAllLocked(); }

void clear() {
    std::lock_guard lock(mutex);
    stopAllLocked();
    // Stopping a handle does not prove that native code has released its buffer.
    static auto& retired = *new std::vector<std::shared_ptr<std::string const>>;
    for (auto& [id, data] : cache) if (data) retired.push_back(std::move(data));
    cache.clear();
}

void enable() {
    hooked = SoundCleanupTickHook::hook() == 0;
    if (!hooked) logger().warn("Cannot hook client END tick; periodic sound cleanup unavailable");
}
void disable() {
    if (hooked) SoundCleanupTickHook::unhook();
    hooked = false;
    stopAllTracked();
}

} // namespace tacz::client::sound
