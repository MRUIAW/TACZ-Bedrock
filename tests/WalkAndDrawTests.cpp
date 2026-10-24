#include <doctest/doctest.h>

#include "gun/DrawRules.h"
#include "model/WalkDistance.h"

using namespace tacz;

TEST_CASE("Draw duration: Java float multiplication before milliseconds truncation") {
    CHECK(gun::animationMillis(.57f) == 570);
    CHECK(gun::animationMillis(.4f) == 400);
    CHECK(gun::animationMillis(-1) == 0);
    CHECK(gun::animationMillis(std::nanf("")) == 0);
}

TEST_CASE("Walk distance: Java horizontal displacement factor, no vertical contribution") {
    CHECK(animation::WalkDistance::increment(3, 4) == doctest::Approx(3));
    CHECK(animation::WalkDistance::increment(-3, -4) == doctest::Approx(3));
    CHECK(animation::WalkDistance::increment(0, 0) == 0);
    CHECK(animation::WalkDistance::increment(.21585, 0) == doctest::Approx(.12951));
    CHECK(animation::WalkDistance::increment(.2806, 0) == doctest::Approx(.16836));
}

TEST_CASE("Walk distance: tick accumulation and TACZ current-plus-delta extrapolation") {
    animation::WalkDistance d;
    d.tick(.2);
    CHECK(d.previous == 0);
    CHECK(d.predicted(0) == doctest::Approx(.12));
    CHECK(d.predicted(.5f) == doctest::Approx(.18));
    CHECK(d.predicted(1) == doctest::Approx(.24));
    for (int frame = 0; frame < 1000; ++frame) d.predicted((frame % 60) / 60.f);
    CHECK(d.current == doctest::Approx(.12)); // rendering never advances the accumulator
    d.tick(.3);
    CHECK(d.previous == doctest::Approx(.12));
    CHECK(d.predicted(.5f) == doctest::Approx(.39));
    d.tick(0);
    CHECK(d.predicted(1) == doctest::Approx(.3));
}

TEST_CASE("Walk distance: one cycle per two Java walk units, independent of render rate") {
    for (int fps : {30, 60, 144}) {
        animation::WalkDistance d;
        for (int tick = 0; tick < 200; ++tick) {
            d.tick(4.317 / 20);
            for (int f = 0; f < fps; ++f) d.predicted(f / static_cast<float>(fps));
        }
        CHECK(d.current / 2 / 10 == doctest::Approx(1.2951).epsilon(.0001));
    }
    animation::WalkDistance sprint;
    for (int tick = 0; tick < 200; ++tick) sprint.tick(5.612 / 20);
    CHECK(sprint.current / 2 / 10 == doctest::Approx(1.6836).epsilon(.0001));
}

TEST_CASE("Hand switch: gun to vanilla retains old gun and locks actions through the handoff") {
    gun::HandSwitch s;
    gun::HandSelection ak{pack::ResourceId{"tacz","ak47"},0}, empty{std::nullopt,1};
    CHECK(s.select(ak, 0, 0, 400) == gun::HandSwitch::Event::Reveal);
    CHECK(s.blocked(399));
    CHECK_FALSE(s.blocked(400));
    CHECK(s.select(empty, 1000, 570, 0) == gun::HandSwitch::Event::PutAway);
    CHECK(s.visible == ak);
    CHECK(s.selected == empty);
    CHECK(s.putAwayEnd == 1570);
    CHECK(s.revealAt == 1620);
    CHECK(s.advance(1619) == gun::HandSwitch::Event::None);
    CHECK(s.blocked(1619));
    CHECK(s.advance(1620) == gun::HandSwitch::Event::Reveal);
    CHECK(s.visible == empty);
    CHECK_FALSE(s.blocked(1620));
}

TEST_CASE("Hand switch: gun to gun draws only after old put-away, latest scroll wins") {
    gun::HandSwitch s;
    gun::HandSelection ak{pack::ResourceId{"tacz","ak47"},0};
    gun::HandSelection m4{pack::ResourceId{"tacz","m4a1"},1};
    gun::HandSelection pistol{pack::ResourceId{"tacz","glock_17"},2};
    s.select(ak, 0, 0, 400);
    s.select(m4, 1000, 570, 500);
    for (int i = 0; i < 100; ++i) {
        s.select(i % 2 == 0 ? m4 : pistol, 1001+i, 570, 300);
        CHECK(s.visible == ak);
        CHECK(s.revealAt == 1620);
    }
    CHECK(s.advance(1620) == gun::HandSwitch::Event::Reveal);
    CHECK(s.visible == pistol);
    CHECK(s.drawUntil == 1920);
    CHECK(s.blocked(1919));
    CHECK_FALSE(s.blocked(1920));
    CHECK(s.advance(2000) == gun::HandSwitch::Event::None);
}

TEST_CASE("Hand switch: scrolling back to the old slot reinitializes only once, world exit clears retention") {
    gun::HandSwitch s;
    gun::HandSelection ak{pack::ResourceId{"tacz","ak47"},0};
    s.select(ak, 0, 0, 400);
    s.select({std::nullopt,1}, 1000, 570, 0);
    s.select(ak, 1050, 570, 400);
    CHECK(s.advance(1620) == gun::HandSwitch::Event::Reveal);
    CHECK(s.visible == ak);
    CHECK(s.drawUntil == 2020);
    s.select({std::nullopt,1}, 2100, 570, 0);
    s.clear();
    CHECK_FALSE(s.visible.gun);
    CHECK_FALSE(s.puttingAway);
    CHECK_FALSE(s.blocked(0));
}

TEST_CASE("Draw cooldown: Java put-away carry-over and rapid switching preserve the future timestamp") {
    gun::DrawCooldown d;
    d.select(0, 570);
    CHECK(d.remaining(0, 400) == 395);
    CHECK(d.remaining(400, 400) == 0);
    d.select(1000, 300);
    CHECK(d.drawTimestamp == 1570);
    CHECK(d.remaining(1000, 500) == 1065);
    d.select(1100, 200);
    CHECK(d.drawTimestamp == 1570); // do not stack put-away durations
    CHECK(d.remaining(1100, 300) == 765);
    d.select(1700, 570);
    CHECK(d.drawTimestamp == 1830); // min(130 ms elapsed draw, old 200 ms put-away)
    CHECK(d.remaining(2230, 400) == 0);
}
