#pragma once

#include <cstdint>

namespace tacz::model {

// GPU handles are not pack assets. Equal addresses in a newly joined world are NOT
// equal owners; pipeline/resource-service replacement also invalidates a live-world cache.
struct RenderResourceKey {
    uint64_t world = 0;
    uintptr_t client = 0, textures = 0, buffers = 0, frameBuilder = 0;
    int lighting = 0;
    bool operator==(RenderResourceKey const&) const = default;
    bool ready() const { return world && client && textures && buffers && frameBuilder; }
};

// Gate BEFORE asking native ClientInstance/level/render services for anything. A cached
// gun/pose or a non-null global client is not evidence that a world currently exists.
inline bool canUseGunRenderResources(uint64_t session, uint64_t snapshot, bool hasGun) {
    return session != 0 && session == snapshot && hasGun;
}

} // namespace tacz::model
