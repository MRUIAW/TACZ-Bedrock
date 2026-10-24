#include <doctest/doctest.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <random>
#include <stdexcept>
#include "model/CameraRecoil.h"
#include "pack/GunPackLoader.h"

using namespace tacz;
namespace {
model::RecoilSpline linear() { return {{0,100,200}, {0,1,2}}; }
model::RecoilSpline returning() { return {{0,100,200}, {0,1,0}}; }
}

TEST_CASE("Recoil: natural cubic reference values, domain and inclusive endpoints") {
    model::RecoilSpline curve{{0,1,2}, {0,1,0}};
    // Commons Math SplineInterpolator's natural solution: on [0,1], 1.5*x-.5*x^3.
    CHECK(curve.evaluate(.5).value() == doctest::Approx(.6875));
    CHECK(curve.evaluate(1.5).value() == doctest::Approx(.6875));
    CHECK(curve.evaluate(0).value() == 0);
    CHECK(curve.evaluate(1).value() == 1);
    CHECK(curve.evaluate(2).value() == 0);
    CHECK_FALSE(curve.evaluate(-.001));
    CHECK_FALSE(curve.evaluate(2.001));
    CHECK_FALSE(curve.evaluate(std::numeric_limits<double>::quiet_NaN()));
    CHECK_THROWS_AS((model::RecoilSpline{{0,1}, {0,1}}), std::invalid_argument);
    CHECK_THROWS_AS((model::RecoilSpline{{0,1,1}, {0,1,0}}), std::invalid_argument);
}

TEST_CASE("Recoil: TACZ keyframes are sorted then shifted +30ms, with a zero origin and independent samples") {
    std::vector<pack::RecoilKeyFrame> keys{{.1f, {1,1}}, {0, {1,1}}};
    std::mt19937 rng{17};
    auto built = model::buildRecoilCurve(keys, 1, rng);
    REQUIRE(built.spline);
    CHECK(built.error.empty());
    auto knots = built.spline->knots();
    REQUIRE(knots.size() == 3);
    CHECK(knots[0] == 0);
    CHECK(knots[1] == 30);
    CHECK(knots[2] == 130);
    CHECK(built.spline->evaluate(0).value() == 0);
    CHECK(built.spline->evaluate(30).value() == doctest::Approx(1));
    CHECK(built.spline->evaluate(130).value() == doctest::Approx(1));
    // +30 is a shifted first knot, NOT a 30ms dead zone followed by a jump.
    CHECK(built.spline->evaluate(15).value() > 0);
    std::reverse(keys.begin(), keys.end());
    std::mt19937 same{17};
    auto sorted = model::buildRecoilCurve(keys, 1, same);
    REQUIRE(sorted.spline);
    CHECK(sorted.spline->evaluate(80).value() == doctest::Approx(built.spline->evaluate(80).value()));
}

TEST_CASE("Recoil: seeded per-shot randomization stays in key ranges; values are sampled, not the curve clamped") {
    std::vector<pack::RecoilKeyFrame> keys{{0, {-.23f,.18f}}, {.35f, {0,0}}};
    std::mt19937 a{12345}, b{12345};
    double old = 100;
    bool varied = false;
    for (int shot = 0; shot < 200; ++shot) {
        auto x = model::buildRecoilCurve(keys, .75f, a);
        auto y = model::buildRecoilCurve(keys, .75f, b);
        REQUIRE(x.spline);
        REQUIRE(y.spline);
        auto first = x.spline->evaluate(30).value();
        CHECK(first >= -.23f * .75f - 1e-7);
        CHECK(first <= .18f * .75f + 1e-7);
        CHECK(first == y.spline->evaluate(30).value());
        CHECK(x.spline->evaluate(380).value() == doctest::Approx(0).epsilon(1e-10));
        if (shot && first != old) varied = true;
        old = first;
    }
    CHECK(varied);
}

TEST_CASE("Recoil: malformed/missing axes do not invent a linear interpolation or crash the other axis") {
    std::mt19937 rng{1};
    auto missing = model::buildRecoilCurve({}, 1, rng);
    CHECK_FALSE(missing.spline);
    CHECK(missing.error.empty());
    for (auto keys : {
        std::vector<pack::RecoilKeyFrame>{{0,{1,1}}},
        std::vector<pack::RecoilKeyFrame>{{0,{1,1}}, {0,{2,2}}},
        std::vector<pack::RecoilKeyFrame>{{-1,{1,1}}, {.1f,{1,1}}},
        std::vector<pack::RecoilKeyFrame>{{0,{2,1}}, {.1f,{1,1}}},
        std::vector<pack::RecoilKeyFrame>{{0,{1}}, {.1f,{1,1}}},
        std::vector<pack::RecoilKeyFrame>{{0,{1,1}}, {std::numeric_limits<float>::infinity(),{1,1}}}
    }) {
        auto result = model::buildRecoilCurve(keys, 1, rng);
        CHECK_FALSE(result.spline);
        CHECK_FALSE(result.error.empty());
    }
}

TEST_CASE("Recoil: ADS zoom modifier caps sqrt at 1.5; crawl and attachments are neutral unless provided") {
    CHECK(model::cameraRecoilModifier(0, 9) == 1);
    CHECK(model::cameraRecoilModifier(1, 1) == 1);
    CHECK(model::cameraRecoilModifier(1, 1.2f) == doctest::Approx(1/std::sqrt(1.2)));
    CHECK(model::cameraRecoilModifier(1, 9) == doctest::Approx(2.0/3));
    CHECK(model::cameraRecoilModifier(.5f, 9) == doctest::Approx(5.0/6));
    CHECK(model::cameraRecoilModifier(1, 9, .5f, .8f) == doctest::Approx((2.0/3)*.5*.8));
    CHECK(model::cameraRecoilModifier(1, .25f) == doctest::Approx(2));
    CHECK(model::cameraRecoilModifier(1, 0) == 1);
    CHECK(model::cameraRecoilModifier(std::numeric_limits<float>::quiet_NaN(), 9) == 1);
}

TEST_CASE("Recoil: delta sampling is frame-rate independent inside domain, without synthetic recovery") {
    model::CameraRecoil fine, coarse;
    fine.shot(returning(), {}, 1000);
    coarse.shot(returning(), {}, 1000);
    double sum = 0;
    for (int i = 0; i <= 50; ++i) sum += fine.update(1000+i).pitch;
    CHECK(sum == doctest::Approx(coarse.update(1050).pitch));
    CHECK(sum == doctest::Approx(-.6875));
    CHECK(coarse.update(1050).pitch == 0); // duplicate frame cannot kick twice
    CHECK(coarse.update(1040).pitch == 0); // bad/backward clock cannot rewind the curve
    sum += fine.update(1200).pitch;
    CHECK(sum == doctest::Approx(0).epsilon(1e-10)); // data returns to 0, not a hardcoded spring
    CHECK(fine.update(1201).pitch == 0);
    CHECK_FALSE(fine.active());

    model::CameraRecoil nonzero;
    nonzero.shot(linear(), {}, 0);
    CHECK(nonzero.update(200).pitch == doctest::Approx(-2));
    CHECK(nonzero.update(201).pitch == 0); // terminal value retained in actual player look
    nonzero.clear();
    CHECK(nonzero.update(300).pitch == 0);

    model::CameraRecoil skipped;
    skipped.shot(returning(), {}, 0);
    CHECK(skipped.update(100).pitch == -1);
    CHECK(skipped.update(201).pitch == 0); // isValidPoint fails: don't force final point after a stall
}

TEST_CASE("Recoil: several overlapping shots replace curves and reset samples, preserving already applied view") {
    model::CameraRecoil camera;
    double pitch = 0, yaw = 0;
    for (int shot = 0; shot < 3; ++shot) {
        camera.shot(linear(), linear(), shot*50);
        auto delta = camera.update(shot*50+50);
        pitch += delta.pitch; yaw += delta.yaw;
        CHECK(delta.pitch == doctest::Approx(-.5));
        CHECK(delta.yaw == doctest::Approx(-.5));
    }
    auto final = camera.update(300);
    pitch += final.pitch; yaw += final.yaw;
    CHECK(pitch == doctest::Approx(-3)); // two partial kicks + last full kick, NOT sum of three full tails
    CHECK(yaw == doctest::Approx(-3));
    camera.shot({}, {}, 400); // absent recoil on the next gun also replaces, no stale tail
    CHECK_FALSE(camera.active());
    CHECK(camera.update(450).pitch == 0);
}

#ifdef TACZ_DEFAULT_PACK_DIR
TEST_CASE("Recoil: real AK47 natural spline agrees with independent Commons Math formula reference table") {
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(std::filesystem::path(TACZ_DEFAULT_PACK_DIR)));
    pack::GunPackRegistry packs;
    packs.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    auto gun = packs.gun({"tacz","ak47"});
    REQUIRE(gun);
    CHECK(gun->data.crawlRecoilMultiplier == doctest::Approx(.5));
    std::mt19937 random{1};
    auto built = model::buildRecoilCurve(gun->data.recoil.pitch, 1, random);
    REQUIRE(built.spline);
    // Computed independently with rational arithmetic using Commons Math 3's mu/z/b/c/d
    // equations at [0,30,130,480,630], values [0,.66,.66,-.175,0]. Float source fields
    // account for the tiny tolerance; overshoot above .66 is intentional natural-cubic behavior.
    std::array<std::pair<double,double>, 10> reference{{
        {0,0}, {15,.35940649046504}, {30,.66}, {80,.96221794728820}, {130,.66},
        {200,.23258411657559}, {330,-.15695868979910}, {480,-.175}, {555,-.09816305620609}, {630,0}
    }};
    for (auto [time, value] : reference) CHECK(built.spline->evaluate(time).value() == doctest::Approx(value).epsilon(1e-6));
    CHECK(built.spline->endMs() == 630);
    auto yaw = model::buildRecoilCurve(gun->data.recoil.yaw, 1, random);
    REQUIRE(yaw.spline);
    CHECK(yaw.spline->endMs() == 380);
}
#endif
