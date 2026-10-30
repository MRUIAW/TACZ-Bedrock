#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include "pack/ResourceId.h"

namespace tacz::sound {

// Java tmpSoundInstance. Replacement does not stop the former sound; callers
// explicitly stop before reload/inspect/draw/put-away. Keyframes never replace it.
class TemporarySound {
public:
    void replace(pack::ResourceId const& sound, uint64_t handle) {
        current = handle ? std::optional<Entry>{{sound, handle}} : std::nullopt;
    }
    void stop(std::function<void(uint64_t)> const& stopHandle, std::optional<pack::ResourceId> expected = {}) const {
        if (current && (!expected || current->sound == *expected)) stopHandle(current->handle);
    }
    void clear() { current.reset(); }
private:
    struct Entry { pack::ResourceId sound; uint64_t handle; };
    std::optional<Entry> current;
};

} // namespace tacz::sound
