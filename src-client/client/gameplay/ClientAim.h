#pragma once

#include <cstdint>
#include <optional>
#include "pack/GunData.h"
#include "pack/GunDisplay.h"

class IClientInstance;
class LocalPlayer;

namespace tacz::client::aim {
struct Snapshot {
    uint64_t world = 0;
    uintptr_t client = 0, player = 0; // comparison-only identities, never dereferenced
    std::optional<pack::ResourceId> gun;
    bool aiming = false;
    float progress = 0, viewProgress = 0, ironZoom = 1, zoomModelFov = 70;
};
void enable();
bool cameraBonesEnabled(); // immutable enable-time config, independent of walking Bob View
void disable();
void clear();
// Input thread writes atomics only. Return true when vanilla use/place must be swallowed.
bool mouse(bool down, bool gameplay, bool gunHeld);
void frame(IClientInstance& client, LocalPlayer& player, std::optional<pack::ResourceId> gun,
    pack::GunData const* data, pack::GunDisplay const* display, bool selectionChanged, bool allowed);
void tick(); // real client tick; no render-frame-dependent progression
Snapshot snapshot(); // mutex-protected, rejected automatically outside the matching world
} // namespace tacz::client::aim
