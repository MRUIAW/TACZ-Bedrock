#include <doctest/doctest.h>

#include <map>
#include <vector>
#include "sound/SoundConcurrency.h"

using namespace tacz;
namespace {
struct Fixture {
    sound::SoundPlaybackState playback;
    std::map<uint64_t, bool> active;
    std::vector<uint64_t> stopped;
    std::vector<size_t> aliases;
    uint64_t next = 0;
    sound::IsPlaying playing = [this](uint64_t h) { return active[h]; };
    sound::StopSound stop = [this](uint64_t h) { stopped.push_back(h); active[h] = false; };
    uint64_t play(pack::ResourceId id = {"fixture", "shot"}, unsigned limit = 4, bool temporary = false) {
        return playback.play(1, id, limit, temporary, playing, stop, [&](size_t slot) {
            aliases.push_back(slot); active[++next] = true; return next;
        });
    }
};
}

TEST_CASE("Sound limiter below and at Java limits stops oldest before playback") {
    for (unsigned limit : {1, 2, 4}) {
        sound::SoundLimiter limiter;
        sound::SoundKey key{1, {"fixture", "shot"}};
        std::map<uint64_t, bool> active;
        std::vector<uint64_t> stopped;
        auto playing = [&](auto h) { return active[h]; };
        auto stop = [&](auto h) { stopped.push_back(h); active[h] = false; };
        for (uint64_t h = 1; h <= limit; ++h) {
            limiter.beforePlay(key, limit, playing, stop);
            CHECK(stopped.empty()); active[h] = true; limiter.track(key, h);
        }
        limiter.beforePlay(key, limit, playing, stop);
        CHECK(stopped == std::vector<uint64_t>{1}); CHECK_FALSE(active[1]);
        CHECK(limiter.trackedCount() == limit - 1);
    }
}

TEST_CASE("Sound limiter prunes inactive entries first and applies active minus limit plus one") {
    sound::SoundLimiter limiter;
    sound::SoundKey key{1, {"fixture", "shot"}};
    std::map<uint64_t, bool> active{{1, false}, {2, true}, {3, true}, {4, false}, {5, true}};
    for (uint64_t h = 1; h <= 5; ++h) limiter.track(key, h);
    std::vector<uint64_t> stopped;
    auto playing = [&](auto h) { return active[h]; };
    auto stop = [&](auto h) { stopped.push_back(h); active[h] = false; };
    limiter.beforePlay(key, 2, playing, stop);
    CHECK(stopped == std::vector<uint64_t>{2, 3}); CHECK(limiter.trackedCount() == 1);
    active[5] = false; stopped.clear(); limiter.beforePlay(key, 1, playing, stop);
    CHECK(stopped.empty()); CHECK(limiter.trackedCount() == 0);
}

TEST_CASE("Sound limiter zero is unlimited and keys are independent per owner and logical sound") {
    sound::SoundLimiter limiter;
    sound::SoundKey a{1, {"fixture", "a"}}, b{1, {"fixture", "b"}}, c{2, {"fixture", "a"}};
    std::vector<uint64_t> stopped;
    auto playing = [](auto) { return true; };
    auto stop = [&](auto h) { stopped.push_back(h); };
    for (uint64_t h = 1; h <= 8; ++h) { limiter.beforePlay(a, 0, playing, stop); limiter.track(a, h); }
    limiter.track(b, 9); limiter.track(c, 10);
    CHECK(stopped.empty()); CHECK(limiter.trackedCount() == 10);
    limiter.beforePlay(a, 4, playing, stop);
    CHECK(stopped == std::vector<uint64_t>{1, 2, 3, 4, 5});
    stopped.clear(); limiter.beforePlay(b, 1, playing, stop); CHECK(stopped == std::vector<uint64_t>{9});
    stopped.clear(); limiter.beforePlay(c, 1, playing, stop); CHECK(stopped == std::vector<uint64_t>{10});
}

TEST_CASE("Sound alias allocation is lazy free first bounded and reuses the FIFO eviction slot") {
    for (unsigned limit : {1, 2, 4}) {
        Fixture f;
        f.play({"fixture", "shot"}, limit);
        for (int i = 0; i < 10; ++i) { f.active[f.next] = false; f.play({"fixture", "shot"}, limit); }
        for (auto slot : f.aliases) CHECK(slot == 0); // no overlap never introduces #1
        for (unsigned i = 1; i < limit; ++i) { f.play({"fixture", "shot"}, limit); CHECK(f.aliases.back() == i); }
        CHECK(f.stopped.empty());
        auto oldest = 11;
        f.play({"fixture", "shot"}, limit);
        CHECK(f.stopped == std::vector<uint64_t>{static_cast<uint64_t>(oldest)});
        CHECK(f.aliases.back() == 0);
    }
    Fixture f;
    for (int i = 0; i < 4; ++i) f.play();
    f.active[3] = false; f.play(); CHECK(f.aliases.back() == 2); CHECK(f.stopped.empty());
    f.play(); CHECK(f.aliases.back() == 0); CHECK(f.stopped == std::vector<uint64_t>{1});
    f.play(); CHECK(f.aliases.back() == 1); CHECK(f.stopped == std::vector<uint64_t>{1, 2});
}

TEST_CASE("Sound aliases share categories per id and capacity grows only to highest category used") {
    Fixture f;
    for (int i = 0; i < 4; ++i) f.play();
    f.play({"fixture", "shot"}, 1);
    CHECK(f.stopped == std::vector<uint64_t>{1, 2, 3, 4}); CHECK(f.aliases.back() == 0);
    f.play({"fixture", "other"}, 2); CHECK(f.aliases.back() == 0);
    f.play({"fixture", "other"}, 2); CHECK(f.aliases.back() == 1);
    f.play({"fixture", "other"}, 2); CHECK(f.aliases.back() == 0);
    sound::SoundAliasPool pool;
    auto playing = [](auto) { return false; };
    CHECK(pool.acquire(1, playing) == 0); CHECK(pool.capacity() == 1); CHECK(pool.allocated() == 1);
    CHECK(pool.acquire(4, playing) == 0); CHECK(pool.capacity() == 4); CHECK(pool.allocated() == 1);
    CHECK(pool.acquire(2, playing) == 0); CHECK(pool.capacity() == 4);
    CHECK_THROWS_AS(pool.acquire(0, playing), std::invalid_argument);
    CHECK_THROWS_AS(f.play({"fixture", "unlimited"}, 0), std::invalid_argument);
    Fixture growing;
    growing.play({"fixture", "shared"}, 1);
    for (int i = 1; i < 4; ++i) { growing.play({"fixture", "shared"}, 4); CHECK(growing.aliases.back() == i); }
    CHECK(growing.stopped.empty());
}

TEST_CASE("Sound aliases are released before a deferred native stop and failed plays do not occupy slots") {
    sound::SoundPlaybackState playback;
    std::vector<size_t> slots;
    std::vector<uint64_t> stopped;
    auto alwaysPlaying = [](auto) { return true; }; // stop has not taken effect yet
    auto stop = [&](auto h) { stopped.push_back(h); };
    auto play = [&](uint64_t handle) {
        return playback.play(1, {"fixture", "shot"}, 1, false, alwaysPlaying, stop, [&](size_t slot) {
            slots.push_back(slot); return handle;
        });
    };
    CHECK(play(1) == 1); CHECK(play(0) == 0); CHECK(play(2) == 2);
    CHECK(slots == std::vector<size_t>{0, 0, 0}); CHECK(stopped == std::vector<uint64_t>{1});
    CHECK(playback.trackedCount() == 1);
}

TEST_CASE("Adapted playback probe verifies overlap with injected engine state not native audibility") {
    Fixture f;
    auto first = f.play(); auto second = f.play();
    CHECK(f.active[first]); CHECK(f.active[second]); CHECK(f.aliases == std::vector<size_t>{0, 1});
    f.playback.cleanup(f.playing, [](auto) { return true; }, f.stop);
    CHECK(f.active[first]); CHECK(f.active[second]); CHECK(f.stopped.empty());
    f.active[first] = false;
    f.playback.cleanup(f.playing, [](auto) { return true; }, f.stop);
    CHECK(f.playback.trackedCount() == 1); CHECK(f.stopped.empty());
}

TEST_CASE("Sound cleanup prunes inactive stops invalid owners and runs every five real client ticks") {
    sound::SoundLimiter limiter;
    limiter.track({1, {"fixture", "a"}}, 1); limiter.track({1, {"fixture", "b"}}, 2);
    limiter.track({2, {"fixture", "a"}}, 3);
    std::vector<uint64_t> stopped;
    limiter.cleanup([](auto h) { return h != 1; }, [](auto owner) { return owner != 1; },
        [&](auto h) { stopped.push_back(h); });
    CHECK(stopped == std::vector<uint64_t>{2}); CHECK(limiter.trackedCount() == 1);
    sound::SoundCleanupCadence cadence;
    for (int tick = 1; tick <= 25; ++tick) CHECK(cadence.tick() == (tick % 5 == 0));
}

TEST_CASE("World engine and no level transitions stop all voices and reset aliases without stale queries") {
    Fixture f;
    f.playback.scope(1, 10, f.stop);
    f.play({"fixture", "shot"}); f.play({"fixture", "keyframe"}); f.play({"fixture", "reload"}, 2, true);
    f.playback.scope(1, 10, f.stop); CHECK(f.stopped.empty());
    f.playback.scope(2, 10, f.stop); CHECK(f.stopped == std::vector<uint64_t>{2, 3, 1});
    CHECK(f.playback.trackedCount() == 0); f.stopped.clear();
    f.play(); CHECK(f.aliases.back() == 0);
    f.playback.scope(2, 20, f.stop); CHECK(f.stopped == std::vector<uint64_t>{4}); f.stopped.clear();
    f.play(); f.playback.scope(0, 0, f.stop); CHECK(f.stopped == std::vector<uint64_t>{5});
    f.stopped.clear(); f.playback.clear(f.stop); CHECK(f.stopped.empty());
}

TEST_CASE("Temporary base slot excludes keyframes and shots and retains Java name filtered stops") {
    Fixture f;
    f.play({"fixture", "reload"}, 2, true);
    f.play({"fixture", "keyframe"}); f.play();
    f.playback.stopTemporary(f.stop); CHECK(f.stopped == std::vector<uint64_t>{1});
    f.stopped.clear(); f.play({"fixture", "inspect"}, 2, true); CHECK(f.stopped.empty());
    f.playback.stopTemporary(f.stop, pack::ResourceId{"fixture", "inspect_empty"}); CHECK(f.stopped.empty());
    f.playback.stopTemporary(f.stop, pack::ResourceId{"fixture", "inspect"}); CHECK(f.stopped == std::vector<uint64_t>{4});
    f.stopped.clear(); f.play({"fixture", "bolt"}, 2, true); f.playback.forgetTemporary();
    f.playback.stopTemporary(f.stop); CHECK(f.stopped.empty());
    f.playback.clear(f.stop); CHECK(f.stopped.size() == 5); // teardown includes all tracked kinds
}

TEST_CASE("Java sound category table covers local gun display keys and animation keyframes") {
    using C = sound::Category;
    CHECK(sound::concurrencyLimit(C::HighFrequency) == 4);
    CHECK(sound::concurrencyLimit(C::Default) == 2); CHECK(sound::concurrencyLimit(C::Hit) == 1);
    for (auto key : {"shoot", "silence"}) CHECK(sound::gunSoundCategory(key) == C::HighFrequency);
    for (auto key : {"head_hit", "flesh_hit"}) CHECK(sound::gunSoundCategory(key) == C::Hit);
    for (auto key : {"dry_fire", "reload_empty", "reload_tactical", "inspect", "inspect_empty", "bolt", "draw",
        "put_away", "fire_select", "melee_bayonet", "melee_push", "melee_stock", "kill", "refit"})
        CHECK(sound::gunSoundCategory(key) == C::Default);
}

TEST_CASE("Temporary replacement does not stop its predecessor and failed base playback clears the slot") {
    Fixture f;
    f.play({"fixture", "reload"}, 2, true);
    f.play({"fixture", "bolt"}, 2, true);
    CHECK(f.stopped.empty()); CHECK(f.active[1]); CHECK(f.active[2]);
    f.playback.stopTemporary(f.stop); CHECK(f.stopped == std::vector<uint64_t>{2}); CHECK(f.active[1]);
    f.stopped.clear();
    f.play({"fixture", "draw"}, 2, true);
    f.playback.play(1, {"fixture", "missing"}, 2, true, f.playing, f.stop, [](size_t) { return uint64_t{0}; });
    f.playback.stopTemporary(f.stop); CHECK(f.stopped.empty()); CHECK(f.active[3]);
}
