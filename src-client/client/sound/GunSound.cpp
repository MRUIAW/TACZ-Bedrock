#include "client/sound/GunSound.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "ll/api/io/Logger.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/sound/SoundEngine.h"
#include "mc/deps/core/file/Path.h"

#include "mod/TaczMod.h"
#include "client/gameplay/ClientShooter.h"
#include "pack/GunDisplay.h"

namespace tacz::client::sound {
namespace {

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

std::mutex mutex;
std::vector<uint64> animationHandles;
Bedrock::NonOwnerPointer<SoundEngine> animationEngine;
uint64_t animationWorld = 0;

// Decoded by the engine from these bytes; they are kept for the process lifetime because the
// engine may stream from the buffer after playFromMemory returns. A null entry marks a missing
// file so it is reported once.
auto& cache  = *new std::map<pack::ResourceId, std::shared_ptr<std::string const>>;
auto& logged = *new std::set<pack::ResourceId>;

std::shared_ptr<std::string const> load(pack::ResourceId const& id) {
    std::lock_guard lock(mutex);
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    auto bytes = TaczMod::getInstance().gunPacks().readFile(pack::assetPath(id, "tacz_sounds", ".ogg"));
    // Packs routinely name sounds they do not ship (the AK47 display's "reload_tactical", animation
    // keyframes like "ar_akilo_drop"); TACZ plays nothing for those, so this is informational.
    if (!bytes) logger().info("Sound {} is not in any gun pack; skipped", id.toString());
    auto data = bytes ? std::make_shared<std::string const>(std::move(*bytes)) : nullptr;
    cache.emplace(id, data);
    return data;
}

} // namespace

bool play(pack::ResourceId const& soundId, float volume, float pitch, bool animation) {
    auto world = shooter::worldToken();
    if (!world) return false;
    auto client = ll::service::getClientInstance();
    if (!client) return false;
    auto data = load(soundId);
    if (!data) return false;
    auto   engine = client->getSoundEngine();
    uint64 handle = engine->playFromMemory(
        Core::Path("tacz/" + soundId.ns + "/" + soundId.path),
        data->data(),
        data->size(),
        false, // not positional: the shooter's own sounds play at the listener
        1.0f,
        16.0f,
        volume,
        pitch,
        false,
        false
    );
    std::lock_guard lock(mutex);
    if (animationWorld != world || animationEngine.get() != &*engine) {
        // Never interpret a previous engine/world's numeric handles in the new one.
        animationHandles.clear();
        animationEngine = Bedrock::NonOwnerPointer<SoundEngine>(*engine);
        animationWorld = world;
    }
    std::erase_if(animationHandles, [&](auto h) { return !engine->isPlayingSound(h); });
    if (animation && handle) animationHandles.push_back(handle);
    if (logged.insert(soundId).second) {
        logger().info("Played sound {} ({} bytes), handle {}", soundId.toString(), data->size(), handle);
    }
    return handle != 0;
}

bool playGunSound(pack::ResourceId const& gunId, std::string const& key, float volume, float pitch, bool animation) {
    auto const& packs = TaczMod::getInstance().gunPacks();
    auto const* gun   = packs.gun(gunId);
    if (!gun || !gun->index.display) return false;
    auto const* display = packs.gunDisplay(*gun->index.display);
    if (!display) return false;
    auto it = display->sounds.find(key);
    if (it == display->sounds.end()) return false;
    return play(it->second, volume, pitch, animation);
}

void stopAnimation() {
    std::lock_guard lock(mutex);
    // No client/engine lookup after teardown, including the empty-handle path on join.
    // NonOwnerPointer invalidates itself if the recorded owner has already been destroyed.
    if (animationWorld && animationWorld == shooter::worldToken()) {
        if (auto* engine = animationEngine.get()) {
            for (auto handle : animationHandles) engine->stop(handle);
        }
    }
    animationHandles.clear();
    animationEngine = {};
    animationWorld = 0;
}

void clear() {
    stopAnimation();
    std::lock_guard lock(mutex);
    // Sounds started from these buffers may still be playing; keep the bytes alive.
    static auto& retired = *new std::vector<std::shared_ptr<std::string const>>;
    for (auto& [id, data] : cache) {
        if (data) retired.push_back(std::move(data));
    }
    cache.clear();
    logged.clear();
}

} // namespace tacz::client::sound
