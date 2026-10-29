#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "model/Animation.h"
#include "model/GunModelRules.h"
#include "pack/ResourceId.h"

// Client adapter for TACZ's Lua animation state machine. All VM/controller logic is shared.
namespace tacz::client::anim {

bool held(std::optional<pack::ResourceId> const& gun, int slot = -1); // true on selection change
void update(); // called only from MinecraftGame::update, never a 50 ms client tick
void shoot();
void bolt();
bool hasStateMachine(); // frame-only presentation availability, never bolt permission
void reload();
void cancelReload();
void inspect();
void fireSelect();
void movementTick();
void captureMovementTick(); // capture each real tick, even with multiple ticks per frame
bool actionsBlocked();
bool aimAllowed(); // Java blocks put-away, not the draw/reload animation itself
void clear();

struct Snapshot {
    uint64_t frame = 0;
    uint64_t world = 0;
    std::optional<pack::ResourceId> gun;
    bool puttingAway = false;
    bool hideCrosshair = false;
    uintptr_t client = 0; // comparison-only render owner, also while keeping put-away item
    bool cameraAllowed = false;
    float cameraAim = 0, cameraZoom = 1;
    animation::Pose pose;
    model::GunRenderState renderState;
    uint64_t        revision = 0;
};
Snapshot snapshot();
struct HudState {
    uint64_t world=0;
    uintptr_t client=0;
    std::optional<pack::ResourceId> gun;
    bool puttingAway=false,hideCrosshair=false;
};
HudState hudState(); // lightweight: do not copy the entire bone pose on every UI pass

} // namespace tacz::client::anim
