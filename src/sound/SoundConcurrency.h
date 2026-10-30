#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>
#include "pack/ResourceId.h"
#include "sound/TemporarySound.h"

namespace tacz::sound {

inline constexpr unsigned HitSoundConcurrencyLimit = 1;
inline constexpr unsigned DefaultSoundConcurrencyLimit = 2;
inline constexpr unsigned HighFrequencySoundConcurrencyLimit = 4;
enum class Category { Default, HighFrequency, Hit };
inline constexpr unsigned concurrencyLimit(Category category) {
    switch (category) {
    case Category::HighFrequency: return HighFrequencySoundConcurrencyLimit;
    case Category::Hit: return HitSoundConcurrencyLimit;
    default: return DefaultSoundConcurrencyLimit;
    }
}
inline constexpr Category gunSoundCategory(std::string_view key) {
    if (key == "shoot" || key == "silence") return Category::HighFrequency;
    if (key == "head_hit" || key == "flesh_hit") return Category::Hit;
    return Category::Default;
}

struct SoundKey {
    uint64_t owner;
    pack::ResourceId sound;
    auto operator<=>(SoundKey const&) const = default;
};
using IsPlaying = std::function<bool(uint64_t)>;
using StopSound = std::function<void(uint64_t)>;

// SoundPlayManager.java:243-281: prune, active-limit+1, oldest ACTIVE first.
// Zero disables enforcement; tracking/cleanup remain available to the caller.
class SoundLimiter {
public:
    void beforePlay(SoundKey const& key, unsigned limit, IsPlaying const& playing, StopSound const& stop) {
        if (!limit) return;
        auto found = sounds.find(key);
        if (found == sounds.end()) return;
        auto& queue = found->second;
        std::erase_if(queue, [&](auto handle) { return !playing(handle); });
        auto toStop = static_cast<int64_t>(queue.size()) - limit + 1;
        for (auto it = queue.begin(); it != queue.end() && toStop > 0;) {
            if (playing(*it)) { stop(*it); it = queue.erase(it); --toStop; }
            else ++it;
        }
        if (queue.empty()) sounds.erase(found);
    }
    void track(SoundKey const& key, uint64_t handle) { if (handle) sounds[key].push_back(handle); }
    void cleanup(IsPlaying const& playing, std::function<bool(uint64_t)> const& ownerValid, StopSound const& stop) {
        for (auto it = sounds.begin(); it != sounds.end();) {
            std::erase_if(it->second, [&](auto handle) {
                if (!playing(handle)) return true;
                if (!ownerValid(it->first.owner)) { stop(handle); return true; }
                return false;
            });
            if (it->second.empty()) it = sounds.erase(it); else ++it;
        }
    }
    void clear(StopSound const& stop) {
        for (auto const& [key, queue] : sounds) for (auto handle : queue) stop(handle);
        sounds.clear();
    }
    size_t trackedCount() const {
        size_t count = 0; for (auto const& [key, queue] : sounds) count += queue.size(); return count;
    }
private:
    std::map<SoundKey, std::deque<uint64_t>> sounds;
};

// The limiter decides eviction, this adapter only finds a now-free name slot.
// Historical per-sound capacity grows to the highest positive category used;
// actual slots/names are created only when no existing slot is free.
class SoundAliasPool {
public:
    size_t acquire(unsigned limit, IsPlaying const& playing) {
        if (!limit) throw std::invalid_argument("unlimited is not a bounded alias-pool category");
        maxLimit = std::max(maxLimit, limit);
        prune(playing);
        for (size_t i = 0; i < slots.size(); ++i) if (!slots[i]) return i;
        if (slots.size() >= maxLimit) throw std::logic_error("sound limiter did not free an alias slot");
        slots.push_back(0);
        return slots.size() - 1;
    }
    void track(size_t slot, uint64_t handle) { slots.at(slot) = handle; }
    void release(uint64_t handle) { for (auto& slot : slots) if (slot == handle) slot = 0; }
    void prune(IsPlaying const& playing) { for (auto& handle : slots) if (handle && !playing(handle)) handle = 0; }
    size_t allocated() const { return slots.size(); }
    unsigned capacity() const { return maxLimit; }
private:
    std::vector<uint64_t> slots;
    unsigned maxLimit = 0;
};

// Called at END of each real client level tick, not from rendered frames.
class SoundCleanupCadence {
public:
    bool tick() { ticks = (ticks + 1) % 5; return ticks == 0; }
private:
    unsigned ticks = 0;
};

// Pure local playback adapter. Aliases are per logical id, never per category.
// The limiter stops before the name is reused, even if native stop is deferred.
class SoundPlaybackState {
public:
    void scope(uint64_t world, uintptr_t engine, StopSound const& stop) {
        if (world == currentWorld && engine == currentEngine) return;
        clear(stop);
        currentWorld = world; currentEngine = engine;
    }
    uint64_t play(uint64_t owner, pack::ResourceId const& id, unsigned limit, bool temporaryBase,
        IsPlaying const& playing, StopSound const& stop, std::function<uint64_t(size_t)> const& playAlias) {
        // No production category is unlimited. Do not pretend a bounded pool can
        // implement Java's configurable zero/unlimited mode.
        if (!limit) throw std::invalid_argument("unlimited playback needs a different native adapter");
        SoundKey key{owner, id};
        auto& pool = pools[id];
        limiter.beforePlay(key, limit, playing, [&](auto handle) { stop(handle); pool.release(handle); });
        auto slot = pool.acquire(limit, playing);
        auto handle = playAlias(slot);
        pool.track(slot, handle);
        limiter.track(key, handle);
        if (temporaryBase) temporary.replace(id, handle);
        return handle;
    }
    void forgetTemporary() { temporary.clear(); }
    void stopTemporary(StopSound const& stop, std::optional<pack::ResourceId> expected = {}) const {
        temporary.stop(stop, expected);
    }
    void cleanup(IsPlaying const& playing, std::function<bool(uint64_t)> const& ownerValid, StopSound const& stop) {
        limiter.cleanup(playing, ownerValid, stop);
        for (auto& [id, pool] : pools) pool.prune(playing);
    }
    void clear(StopSound const& stop) {
        limiter.clear(stop); pools.clear(); temporary.clear();
        currentWorld = 0; currentEngine = 0;
    }
    size_t trackedCount() const { return limiter.trackedCount(); }
private:
    SoundLimiter limiter;
    std::map<pack::ResourceId, SoundAliasPool> pools;
    TemporarySound temporary;
    uint64_t currentWorld = 0;
    uintptr_t currentEngine = 0;
};

} // namespace tacz::sound
