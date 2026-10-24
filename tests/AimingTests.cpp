#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <glm/gtc/matrix_transform.hpp>
#include "model/Aiming.h"
#include "model/GunModelRules.h"
#include "gun/ShootRules.h"

using namespace tacz;
namespace {
void matrixNear(glm::mat4 const& a, glm::mat4 const& b) {
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        CHECK(a[c][r] == doctest::Approx(b[c][r]).epsilon(.0001));
}
}
TEST_CASE("ADS: real-tick progress uses elapsed milliseconds plus one, clamps, reverses and snaps zero duration") {
    float p = 0;
    CHECK(model::advanceAimProgress(p, true, 50, .2f) == doctest::Approx(.255f));
    for (int i = 0; i < 4; ++i) p = model::advanceAimProgress(p, true, 50, .2f);
    CHECK(p == 1);
    CHECK(model::fullyAimed(p));
    p = model::advanceAimProgress(p, false, 50, .2f);
    CHECK(p == doctest::Approx(.745));
    CHECK_FALSE(model::fullyAimed(p));
    for (int i = 0; i < 4; ++i) p = model::advanceAimProgress(p, false, 50, .2f);
    CHECK(p == 0);
    CHECK(model::advanceAimProgress(.4f, true, 0, 0) == 1);
    CHECK(model::advanceAimProgress(.4f, false, 0, -1) == 0);
    CHECK(model::advanceAimProgress(.4f, true, -1, .2f) == doctest::Approx(.4));
    CHECK(model::advanceAimProgress(.4f, true, 100000, .2f) == 1);
    CHECK(model::effectiveAdsTime(.2f, .4f) == doctest::Approx(.4));
    CHECK(model::effectiveAdsTime(.2f, -.1f) == 0);
    auto nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(model::effectiveAdsTime(nan) == doctest::Approx(.2));
    CHECK_FALSE(model::fullyAimed(nan));
    CHECK(model::advanceAimProgress(.4f, true, nan, .2f) == doctest::Approx(.4));
}
TEST_CASE("ADS: magnification is a tangent ratio, not FOV divided by zoom; sensitivity follows MDV133") {
    CHECK(model::aimingFov(70, 1.2f, 0) == doctest::Approx(70));
    auto fov = model::aimingFov(70, 1.2f, 1);
    CHECK(fov > 60);
    CHECK(fov < 61);
    CHECK(std::tan(glm::radians(70.f) / 2) / std::tan(glm::radians(fov) / 2) == doctest::Approx(1.2));
    auto half = model::aimingFov(90, 3, .5f);
    CHECK(half == doctest::Approx(model::magnificationToFov(90, 2)));
    CHECK(model::aimingSensitivity(70, 1.2f, 0) == doctest::Approx(1));
    CHECK(model::aimingSensitivity(70, 1.2f, 1) > .8f);
    CHECK(model::aimingSensitivity(70, 1.2f, 1) < 1);
    CHECK(model::aimingFov(70, 0, 1) == doctest::Approx(70));
    CHECK(model::aimingFov(70, 2, -1) == doctest::Approx(70));
    CHECK(model::magnificationToFov(70, -1) == 70);
    CHECK(model::magnificationToFov(0, 2) == 0);
}
TEST_CASE("ADS: deterministic second-order dynamics matches TACZ integration and survives stalls/reset") {
    model::AimDynamics d;
    CHECK(d.update(1, 0) == 0);
    constexpr float pi = 3.14159265358979323846f;
    float k2 = 1.f / std::pow(2 * pi * 1.2f, 2), k3 = .5f * 1.2f / (2 * pi * 1.2f);
    CHECK(d.update(1, .006) == doctest::Approx(.05f * k3 / k2));
    model::AimDynamics a, b;
    float av = 0, bv = 0;
    for (int i = 0; i < 100; ++i) av = a.update(1, .006);
    for (int i = 0; i < 50; ++i) bv = b.update(1, .012);
    CHECK(av == doctest::Approx(bv));
    CHECK(av == doctest::Approx(1).epsilon(.001));
    CHECK(std::isfinite(a.update(0, 1e8)));
    CHECK(std::isfinite(a.update(std::numeric_limits<float>::quiet_NaN(), .006)));
    a.reset(70);
    CHECK(a.update(70, .1) == doctest::Approx(70));
}
TEST_CASE("ADS: inverse view translation and shortest-arc rotation blend; missing iron retains idle") {
    auto idle = glm::translate(glm::mat4(1), {1,2,3});
    auto iron = glm::rotate(glm::translate(glm::mat4(1), {-1,4,1}), glm::radians(90.f), {0,1,0});
    matrixNear(model::blendAimViews(idle, iron, 0), idle);
    matrixNear(model::blendAimViews(idle, iron, 1), iron);
    auto half = model::blendAimViews(idle, iron, .5f);
    CHECK(half[3].x == doctest::Approx(0));
    CHECK(half[3].y == doctest::Approx(3));
    CHECK(half[3].z == doctest::Approx(2));
    CHECK(half[0][0] == doctest::Approx(std::sqrt(.5f)));
    CHECK(glm::determinant(half) == doctest::Approx(1));
    matrixNear(model::blendAimViews(idle, idle, .5f), idle);
    model::GeoModel geo;
    matrixNear(model::firstPersonAimTransform(glm::mat4(1), glm::mat4(1), 0), model::firstPersonIdleTransform(geo));
}
TEST_CASE("ADS: model FOV edits only projection x/y scale and retains aspect/depth/offcentre") {
    auto p = glm::perspective(glm::radians(70.f), 16.f / 9, .05f, 1000.f);
    p[2][0] = .02f;
    auto zoom = model::withModelFov(p, 55);
    CHECK(model::projectionFov(p) == doctest::Approx(70));
    CHECK(model::projectionFov(zoom) == doctest::Approx(55));
    CHECK(zoom[1][1] / zoom[0][0] == doctest::Approx(16.f / 9));
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        if (!(c == r && c < 2)) CHECK(zoom[c][r] == p[c][r]);
    matrixNear(model::withModelFov(p, 70), p);
    matrixNear(model::withModelFov(glm::mat4(1), 55), glm::mat4(1));
    matrixNear(model::withModelFov(p, 180), p);
}
TEST_CASE("ADS: AIM accuracy only at full server progress; reload speed overrides aim speed") {
    using pack::InaccuracyType;
    CHECK(gun::inaccuracyType(model::fullyAimed(.9999f), false, 0) == InaccuracyType::Stand);
    CHECK(gun::inaccuracyType(model::fullyAimed(.5f), true, 0) == InaccuracyType::Sneak);
    CHECK(gun::inaccuracyType(model::fullyAimed(.5f), false, .2) == InaccuracyType::Move);
    CHECK(gun::inaccuracyType(model::fullyAimed(1), true, .2) == InaccuracyType::Aim);
    pack::MoveSpeed speeds{0, -.2f, -.15f};
    CHECK(model::movementModifier(speeds, false, false) == 0);
    CHECK(model::movementModifier(speeds, true, false) == doctest::Approx(-.2));
    CHECK(model::movementModifier(speeds, true, true) == doctest::Approx(-.15));
}
