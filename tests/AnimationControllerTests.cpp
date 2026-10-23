#include <doctest/doctest.h>

#include <numbers>

#include "model/AnimationController.h"
#include "pack/GunPackParser.h"

using namespace tacz;
namespace {
animation::AnimationFile clips() {
    return animation::parseAnimationFile(pack::parseJsonText(R"({"animations":{
      "a":{"bones":{"root":{"position":{"0":[0,0,0],"1":[16,0,0]},"scale":2}}},
      "b":{"bones":{"root":{"position":[32,0,0],"scale":4}},"sound_effects":{"1":{"effect":"tacz:b"}}},
      "r1":{"bones":{"root":{"rotation":[0,0,170]}}},
      "r2":{"bones":{"root":{"rotation":[0,0,-170]}}},
      "kick":{"bones":{"root":{"position":[16,0,0],"scale":2}},"sound_effects":{"1":{"effect":"tacz:k"}}},
      "partial":{"bones":{"bolt":{"position":[0,16,0]}},"sound_effects":{"1":{"effect":"tacz:p"}}}
    }})"));
}
float x(animation::Pose const& p) { return p.at("root").translation->x; }
}

TEST_CASE("Controller: strict loop hold stop boundaries, terminal stop frame and idle reuse") {
    auto a = clips();
    animation::Controller c(a);
    int line = c.addTrackLine(), track = c.assignNewTrack(line);
    c.runAnimation("a", track, false, animation::PlayType::Loop, 0);
    CHECK(x(c.update(1)) == doctest::Approx(1));
    CHECK(x(c.update(1.25)) == doctest::Approx(.25));
    c.setTime(2);
    c.runAnimation("a", track, false, animation::PlayType::Hold, 0);
    c.update(3);
    CHECK_FALSE(c.info(track).holding);
    CHECK(x(c.update(3.1)) == doctest::Approx(1));
    CHECK(c.info(track).holding);
    CHECK(c.findIdleTrack(line, true) == track);
    CHECK(c.findIdleTrack(line, false) != track);
    c.setTime(4);
    c.runAnimation("a", track, false, animation::PlayType::Stop, 0);
    CHECK(x(c.update(5.1)) == doctest::Approx(1)); // TACZ updates once while becoming stopped
    CHECK(c.info(track).stopped);
    CHECK(c.update(5.2).empty());
    CHECK(c.findIdleTrack(line, false) == track);
}

TEST_CASE("Controller: lines compose by line order, not allocation order; kicks overlap") {
    auto a = clips();
    animation::Controller c(a);
    c.ensureTrackLineSize(3);
    int movement = c.assignNewTrack(2); // deliberately allocated first
    int base = c.assignNewTrack(0);
    int kick1 = c.assignNewTrack(1), kick2 = c.assignNewTrack(1);
    c.runAnimation("a", base, false, animation::PlayType::Loop, 0);
    c.runAnimation("kick", kick1, true, animation::PlayType::Stop, 0);
    c.runAnimation("kick", kick2, true, animation::PlayType::Stop, 0);
    auto p = c.update(.5);
    CHECK(x(p) == doctest::Approx(2.5));
    CHECK(p.at("root").scale->x == doctest::Approx(8));
    c.runAnimation("b", movement, false, animation::PlayType::Loop, 0);
    CHECK(x(c.update(.5)) == doctest::Approx(2)); // highest line overwrites both kick tracks
    CHECK(c.getTrack(2, 0) == movement);
    CHECK(c.getAsSingletonTrack(2) == movement);
    CHECK(c.getTrack(9, 0) == -1);
}

TEST_CASE("Controller: transition easing is 1-(1-t)^4 and target runs during fade") {
    auto a = clips();
    animation::Controller c(a);
    c.addTrackLine(); int t = c.assignNewTrack(0);
    c.runAnimation("a", t, false, animation::PlayType::Loop, 0);
    c.update(0);
    c.runAnimation("b", t, false, animation::PlayType::Loop, 1);
    auto p = c.update(.5);
    CHECK(x(p) == doctest::Approx(1.875)); // 0 -> 2 at alpha=.9375
    CHECK(p.at("root").scale->x == doctest::Approx(3.875));
    CHECK(c.info(t).name == "b");
    CHECK(c.info(t).progress == doctest::Approx(.5));
    CHECK(c.info(t).transitioning);
    CHECK(x(c.update(1)) == doctest::Approx(2));
    CHECK_FALSE(c.info(t).transitioning);
}

TEST_CASE("Controller: transition rotation uses shortest-path quaternion slerp") {
    auto a = clips();
    animation::Controller c(a);
    c.addTrackLine(); int t = c.assignNewTrack(0);
    c.runAnimation("r1", t, false, animation::PlayType::Loop, 0);
    c.update(0);
    c.runAnimation("r2", t, false, animation::PlayType::Loop, 1);
    auto p = c.update(.5);
    CHECK(p.at("root").rotation->z == doctest::Approx(-171.25 * std::numbers::pi / 180).epsilon(.0001));
}

TEST_CASE("Controller: transition recovery is skipped for PLAY_ONCE_STOP and new channels apply immediately") {
    auto a = clips();
    for (auto type : {animation::PlayType::Hold, animation::PlayType::Stop}) {
        animation::Controller c(a);
        c.addTrackLine(); int t = c.assignNewTrack(0);
        c.runAnimation("b", t, false, type, 0);
        c.update(0);
        c.runAnimation("partial", t, false, animation::PlayType::Loop, 1);
        auto p = c.update(.5);
        CHECK(p.at("bolt").translation->y == doctest::Approx(1)); // y-up; flipped by the model listener later
        if (type == animation::PlayType::Hold) {
            CHECK(x(p) == doctest::Approx(.125));
            CHECK(p.at("root").scale->x == doctest::Approx(1.1875));
        } else CHECK_FALSE(p.contains("root"));
    }
}

TEST_CASE("Controller: interrupted transitions snapshot the current value") {
    auto a = clips();
    animation::Controller c(a);
    c.addTrackLine(); int t = c.assignNewTrack(0);
    c.runAnimation("a", t, false, animation::PlayType::Loop, 0);
    c.update(0);
    c.runAnimation("b", t, false, animation::PlayType::Loop, 1);
    CHECK(x(c.update(.5)) == doctest::Approx(1.875));
    c.runAnimation("kick", t, false, animation::PlayType::Loop, 1);
    CHECK(x(c.update(.5)) == doctest::Approx(1.875));
    CHECK(x(c.update(1)) == doctest::Approx(1.0546875));
}

TEST_CASE("Controller: pause resume seek and progress on transition targets") {
    auto a = clips();
    animation::Controller c(a);
    c.addTrackLine(); int t = c.assignNewTrack(0);
    c.runAnimation("a", t, false, animation::PlayType::Loop, 0);
    c.update(.25);
    c.pauseAnimation(t);
    CHECK(x(c.update(1)) == doctest::Approx(.25));
    c.setAnimationProgress(t, .5f, true);
    CHECK(x(c.update(1)) == doctest::Approx(.5));
    c.adjustAnimationProgress(t, .1f, false); // paused root is not adjusted, per Java
    CHECK(c.info(t).progress == doctest::Approx(.5));
    c.resumeAnimation(t);
    CHECK(x(c.update(1.1)) == doctest::Approx(.6));
    c.runAnimation("a", t, false, animation::PlayType::Loop, .5f);
    c.setAnimationProgress(t, .75f, true);
    c.adjustAnimationProgress(t, -.25f, false);
    CHECK(c.info(t).progress == doctest::Approx(.5));
    c.holdAnimation(t); // root, not the transition target
    CHECK_FALSE(c.info(t).holding);
}

TEST_CASE("Controller: sounds come from advancing runners, including transition targets") {
    auto a = clips();
    std::vector<std::string> sounds;
    animation::Controller c(a, [&](auto const& name, double, double) { sounds.push_back(name); });
    c.addTrackLine(); int t = c.assignNewTrack(0);
    c.runAnimation("a", t, false, animation::PlayType::Loop, 0);
    c.update(.1);
    sounds.clear();
    c.runAnimation("b", t, false, animation::PlayType::Loop, 1);
    c.update(.2);
    REQUIRE(sounds.size() == 1);
    CHECK(sounds[0] == "b");
    c.runAnimation("missing", t, false, animation::PlayType::Stop, 0);
    CHECK(c.info(t).name == "b");
    CHECK_THROWS(c.ensureTrackLineSize(1000));
}

TEST_CASE("Controller: a missing prototype still updates the track blending flag") {
    auto a = clips();
    animation::Controller c(a);
    c.addTrackLine(); int base = c.assignNewTrack(0), top = c.assignNewTrack(0);
    c.runAnimation("b", base, false, animation::PlayType::Loop, 0);
    c.runAnimation("kick", top, false, animation::PlayType::Loop, 0);
    CHECK(x(c.update(.1)) == doctest::Approx(1));
    c.runAnimation("missing", top, true, animation::PlayType::Stop, 0);
    CHECK(x(c.update(.2)) == doctest::Approx(3));
}
