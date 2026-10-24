#include <doctest/doctest.h>

#include "model/RenderResourceKey.h"

using tacz::model::RenderResourceKey;
using tacz::model::canUseGunRenderResources;

TEST_CASE("Render access: menu and stale gun snapshots never authorize world resource queries") {
    CHECK_FALSE(canUseGunRenderResources(0, 0, false));
    CHECK_FALSE(canUseGunRenderResources(0, 3, true)); // cached gun while at the menu
    CHECK_FALSE(canUseGunRenderResources(3, 0, true)); // snapshot not yet initialized
    CHECK_FALSE(canUseGunRenderResources(5, 3, true)); // new world, old snapshot
    CHECK_FALSE(canUseGunRenderResources(3, 3, false));
    CHECK(canUseGunRenderResources(3, 3, true));
    CHECK_FALSE(canUseGunRenderResources(0, 3, true)); // after leave
}

TEST_CASE("Render access: a live world with a partial pipeline defers cache construction") {
    RenderResourceKey ready{3, 100, 200, 300, 400, 0};
    CHECK(ready.ready());
    CHECK_FALSE(RenderResourceKey{}.ready());
    for (int field = 0; field < 5; ++field) {
        auto partial = ready;
        switch (field) {
        case 0: partial.world = 0; break;
        case 1: partial.client = 0; break;
        case 2: partial.textures = 0; break;
        case 3: partial.buffers = 0; break;
        case 4: partial.frameBuilder = 0; break;
        }
        CHECK_FALSE(partial.ready());
    }
}

TEST_CASE("Render cache: reused client/texture/buffer addresses do not survive a world change") {
    RenderResourceKey first{3, 100, 200, 300, 400, 0};
    CHECK(first == RenderResourceKey{3, 100, 200, 300, 400, 0});
    auto second = first;
    second.world = 5;
    CHECK_FALSE(first == second);
    auto third = second;
    third.world = 7;
    CHECK_FALSE(second == third);
    CHECK_FALSE(first == third);
}

TEST_CASE("Render cache: graphics and resource replacements invalidate even in the same world") {
    RenderResourceKey original{3, 100, 200, 300, 400, 0};
    for (int field = 0; field < 5; ++field) {
        auto changed = original;
        switch (field) {
        case 0: ++changed.client; break;
        case 1: ++changed.textures; break;
        case 2: ++changed.buffers; break;
        case 3: ++changed.frameBuilder; break;
        case 4: ++changed.lighting; break;
        }
        CHECK_FALSE(original == changed);
    }
}
