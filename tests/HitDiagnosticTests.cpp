#include <doctest/doctest.h>
#include <random>
#include "debug/HitDiagnostic.h"

using namespace tacz;

TEST_CASE("Hit diagnostic is opt in bounded and invalidates old tags on stop or rearm") {
    debug::hit::Budget budget;
    CHECK_FALSE(budget.trigger(1, 2)); CHECK_FALSE(budget.take({})); CHECK_FALSE(budget.available());
    budget.arm(2);
    auto first = budget.trigger(10, 100); REQUIRE(first);
    CHECK(first->shot == 1); CHECK(first->tick == 10); CHECK(first->timestamp == 100);
    auto second = budget.trigger(11, 150); REQUIRE(second); CHECK(second->shot == 2);
    CHECK_FALSE(budget.trigger(12, 200));
    for (unsigned i = 0; i < debug::hit::LineLimit; ++i) CHECK(budget.take(*first));
    CHECK_FALSE(budget.take(*second)); CHECK_FALSE(budget.available()); CHECK(budget.used() == 64);
    budget.arm(1000);
    CHECK_FALSE(budget.take(*first));
    for (unsigned i = 0; i < debug::hit::ShotLimit; ++i) CHECK(budget.trigger(i, i));
    CHECK_FALSE(budget.trigger(20, 20));
    auto fresh = budget.trigger(0, 0).value_or(debug::hit::Tag{}); // default tag remains invalid
    CHECK_FALSE(budget.take(fresh));
    budget.arm(1); auto final = budget.trigger(1, 1); REQUIRE(final);
    budget.arm(0); CHECK_FALSE(budget.take(*final)); CHECK_FALSE(budget.trigger(2, 2));
}

TEST_CASE("Hit diagnostic position history requires consecutive phase samples and never invents prior positions") {
    debug::hit::PositionHistory history;
    CHECK_FALSE(history.previousPosition(1, 1));
    history.sample(5, {{1, {1, 2, 3}}}); CHECK_FALSE(history.previousPosition(1, 5));
    history.sample(6, {{1, {2, 2, 3}}, {2, {0, 0, 0}}});
    REQUIRE(history.previousPosition(1, 6)); CHECK(*history.previousPosition(1, 6) == glm::dvec3{1, 2, 3});
    CHECK_FALSE(history.previousPosition(2, 6)); CHECK_FALSE(history.previousPosition(1, 7));
    history.sample(8, {{1, {3, 2, 3}}}); CHECK_FALSE(history.previousPosition(1, 8));
    history.sample(9, {}); CHECK_FALSE(history.previousPosition(1, 9));
    history.clear(); CHECK_FALSE(history.previousPosition(1, 9));
    CHECK(debug::hit::midpointEye({4, 11.6, 0}, {4, 10, 0}, {2, 10, 0}) == glm::dvec3{3, 11.6, 0});
}

TEST_CASE("Diagnostic segment box distance handles entry grazing inside parallel zero length and endpoint minima") {
    gun::Box box{{0, 0, 0}, {1, 1, 1}};
    auto distance = [&](glm::dvec3 a, glm::dvec3 b) { return debug::hit::segmentBoxDistance(box, a, b); };
    CHECK(distance({-1, 0.5, 0.5}, {2, 0.5, 0.5}) == 0);
    CHECK(distance({0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}) == 0);
    CHECK_FALSE(gun::clip(box, {0.5, 0.5, 0.5}, {0.5, 0.5, 0.5}));
    CHECK(distance({-1, 1, 1}, {2, 1, 1}) == 0);
    CHECK(distance({-1, 2, 0.5}, {2, 2, 0.5}) == doctest::Approx(1));
    CHECK(distance({2, 2, 2}, {3, 3, 3}) == doctest::Approx(std::sqrt(3.0)));
    CHECK(distance({2, 2, 2}, {2, 2, 2}) == doctest::Approx(std::sqrt(3.0)));
    CHECK(distance({-1, 2, 0.5}, {2, 2, 0.5}) == distance({2, 2, 0.5}, {-1, 2, 0.5}));
    // Interior minimum on an oblique line, not an endpoint or slab intersection.
    CHECK(distance({-2, 0, 0.5}, {0, 2, 0.5}) == doctest::Approx(std::sqrt(0.5)));
}

TEST_CASE("Diagnostic segment box distance is symmetric translation invariant and no larger than sampled distances") {
    std::mt19937 rng{17}; std::uniform_real_distribution<double> random{-5, 5};
    gun::Box box{{-0.4, -1, 0}, {0.6, 2, 0.8}};
    for (int trial = 0; trial < 100; ++trial) {
        glm::dvec3 a{random(rng), random(rng), random(rng)}, b{random(rng), random(rng), random(rng)};
        glm::dvec3 offset{7, -3, 4};
        double d = debug::hit::segmentBoxDistance(box, a, b);
        CHECK(d == doctest::Approx(debug::hit::segmentBoxDistance(box, b, a)));
        CHECK(d == doctest::Approx(debug::hit::segmentBoxDistance(box.move(offset), a + offset, b + offset)));
        for (int step = 0; step <= 100; ++step) {
            auto p = a + (b - a) * (step / 100.0);
            auto gap = glm::max(glm::max(box.min - p, p - box.max), glm::dvec3{0});
            CHECK(d <= glm::length(gap) + 1e-10);
        }
    }
}
