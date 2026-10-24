#include <doctest/doctest.h>

#include "gun/ClientWorldSession.h"

using tacz::gun::ClientWorldSession;

TEST_CASE("Client world lifetime: a not-yet-loaded or departed player cannot accept work") {
    ClientWorldSession world;
    CHECK(world.current() == 0);
    CHECK_FALSE(world.accepts(0));
    world.close();
    CHECK(world.current() == 0);
    auto joined = world.open();
    REQUIRE(joined != 0);
    CHECK(world.accepts(joined));
    world.close();
    CHECK(world.current() == 0);
    CHECK_FALSE(world.accepts(joined));
}

TEST_CASE("Client world lifetime: queued reload/debug/tick work cannot cross leave-rejoin") {
    ClientWorldSession world;
    auto first = world.open();
    world.close();
    auto second = world.open();
    CHECK(first != second);
    CHECK_FALSE(world.accepts(first));
    CHECK(world.accepts(second));
    // Even an open with no intervening exit invalidates a replaced world's work.
    auto replacement = world.open();
    CHECK(replacement != second);
    CHECK_FALSE(world.accepts(second));
    CHECK(world.accepts(replacement));
    world.close();
    world.close();
    CHECK_FALSE(world.accepts(replacement));
    auto third = world.open();
    CHECK_FALSE(world.accepts(first));
    CHECK_FALSE(world.accepts(second));
    CHECK(world.accepts(third));
}
