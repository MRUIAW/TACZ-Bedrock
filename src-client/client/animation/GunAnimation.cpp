#include "client/animation/GunAnimation.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <utility>

#include "ll/api/io/Logger.h"
#include "mc/deps/core/utility/optional_ref.h"
#include "ll/api/network/packet/Packet.h"

#include "client/sound/GunSound.h"
#include "mod/TaczMod.h"
#include "net/DebugPackets.h"
#include "pack/AnimationSounds.h"
#include "pack/GunAnimations.h"
#include "pack/GunPackParser.h"

namespace tacz::client::anim {
namespace {
using Clock = std::chrono::steady_clock;

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

struct Clips {
    animation::AnimationFile animations;
    std::map<std::string, std::vector<pack::AnimationSound>> sounds;
};
struct Track {
    std::string name;
    Clock::time_point start;
    animation::PlayType type = animation::PlayType::Stop;
    bool sounds = false;
    double previous = -1;
};

// Tracks/cache belong to the client frame thread. Only requests and the published snapshot
// cross packet/render threads; do not read the held item or touch the renderer in a handler.
std::map<pack::ResourceId, Clips> cache;
std::optional<pack::ResourceId> currentGun;
int currentSlot = -1;
Clips const* clips = nullptr;
Clock::time_point idleStart;
std::optional<Track> action, kick;
bool reloading = false;
std::mutex mutex;
std::optional<std::pair<std::string, std::string>> request;
Snapshot published;
std::optional<pack::ResourceId> publishedGun;

void play(std::optional<Track>& track, std::string const& name, animation::PlayType type, bool sounds = false) {
    if (!clips || !clips->animations.contains(name)) return;
    track = Track{name, Clock::now(), type, sounds};
}

void sample(std::optional<Track>& track, Clock::time_point now, animation::Pose& pose, bool blend) {
    if (!track || !clips) return;
    auto const& clip = clips->animations.at(track->name);
    double elapsed = std::chrono::duration<double>(now - track->start).count();
    if (track->sounds) {
        if (auto it = clips->sounds.find(track->name); it != clips->sounds.end()) {
            // Events use unwrapped time; a loop wrap emits both the tail and the next head.
            for (auto const& event : it->second) {
                double eventTime = event.time;
                if (track->type == animation::PlayType::Loop && clip.duration > 0) {
                    double lap = std::max(0.0, std::floor((track->previous - eventTime) / clip.duration) + 1);
                    eventTime += lap * clip.duration;
                }
                if (eventTime > track->previous && eventTime <= elapsed) sound::play(event.sound, 1.f, 1.f);
            }
        }
    }
    track->previous = elapsed;
    auto time = animation::playbackTime(clip, elapsed, track->type);
    if (!time) {
        track.reset();
        return;
    }
    animation::evaluate(clip, *time, pose, blend);
}

class AnimationDebugHandler final : public ll::network::PacketHandlerBase<AnimationDebugHandler, net::AnimationDebugPacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, net::AnimationDebugPacket const& packet) const {
        std::lock_guard lock(mutex);
        request = std::pair{packet.gunId, packet.name};
    }
};
} // namespace

bool held(std::optional<pack::ResourceId> const& gun, int slot) {
    if (gun == currentGun && slot == currentSlot) return false;
    currentGun = gun;
    currentSlot = slot;
    action.reset();
    kick.reset();
    reloading = false;
    clips = nullptr;
    idleStart = Clock::now();
    if (!gun) return true;
    auto const& packs = TaczMod::getInstance().gunPacks();
    auto entry = packs.gun(*gun);
    if (!entry || !entry->index.display) return true;
    auto const& displayId = *entry->index.display;
    auto [it, inserted] = cache.try_emplace(displayId);
    if (inserted) {
        if (auto display = packs.gunDisplay(displayId)) {
            it->second.animations = pack::loadGunAnimations(packs, *display, [](auto const& msg) { logger().warn("{}", msg); });
            if (display->animation) {
                if (auto text = packs.readFile(pack::assetPath(*display->animation, "animations", ".animation.json"))) {
                    try { it->second.sounds = pack::parseAnimationSounds(pack::parseJsonText(*text)); }
                    catch (std::exception const& e) { logger().warn("Animation sounds: {}", e.what()); }
                }
            }
            logger().info("[anim] {}: {} clips (temporary C++ driver)", displayId.toString(), it->second.animations.size());
        }
    }
    clips = &it->second;
    play(action, "draw", animation::PlayType::Stop, true);
    return true;
}

void update() {
    std::optional<std::pair<std::string, std::string>> debug;
    {
        std::lock_guard lock(mutex);
        debug.swap(request);
    }
    if (debug && clips && currentGun && debug->first == currentGun->toString()) {
        auto it = clips->animations.find(debug->second);
        if (it == clips->animations.end()) {
            std::string names;
            for (auto const& [name, clip] : clips->animations) names += (names.empty() ? "" : ", ") + name;
            logger().info("[anim] Unknown '{}'. Available: {}", debug->second, names);
        } else {
            kick.reset();
            reloading = false;
            play(action, debug->second, it->second.playType, true);
            logger().info("[anim] Playing {} ({:.3f}s)", debug->second, it->second.duration);
        }
    }
    animation::Pose pose;
    auto now = Clock::now();
    if (clips) {
        if (auto idle = clips->animations.find("static_idle"); idle != clips->animations.end()) {
            auto time = animation::playbackTime(idle->second, std::chrono::duration<double>(now - idleStart).count(), animation::PlayType::Loop);
            animation::evaluate(idle->second, *time, pose);
        }
        sample(action, now, pose, false);
        sample(kick, now, pose, true); // GUN_KICK: additive translation/Euler, multiplicative scale
    }
    // Camera/constraint listeners are deferred to 0.3.7, not ordinary model listeners.
    pose.erase("camera");
    pose.erase("constraint");
    std::lock_guard lock(mutex);
    if (pose != published.pose || currentGun != publishedGun) {
        published.pose = std::move(pose);
        ++published.revision;
        publishedGun = currentGun;
    }
}

void shoot() { play(kick, "shoot", animation::PlayType::Stop); }
void reload(bool empty) {
    kick.reset();
    reloading = true;
    play(action, empty ? "reload_empty" : "reload_tactical", animation::PlayType::Stop);
}
void cancelReload() {
    if (reloading) action.reset();
    reloading = false;
}

Snapshot snapshot(pack::ResourceId const& gun) {
    std::lock_guard lock(mutex);
    return publishedGun && *publishedGun == gun ? published : Snapshot{};
}

void clear() {
    held(std::nullopt);
    cache.clear();
    std::lock_guard lock(mutex);
    request.reset();
    published.pose.clear();
    ++published.revision;
    publishedGun.reset();
}

} // namespace tacz::client::anim
