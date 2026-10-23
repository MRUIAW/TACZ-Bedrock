#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "model/Animation.h"
#include "pack/ResourceId.h"

// Client adapter for TACZ's Lua animation state machine. All VM/controller logic is shared.
namespace tacz::client::anim {

bool held(std::optional<pack::ResourceId> const& gun, int slot = -1); // true on selection change
void update(); // called only from MinecraftGame::update, never a 50 ms client tick
void shoot();
void reload();
void cancelReload();
void inspect();
void fireSelect();
void movementTick();
void clear();

struct Snapshot {
    animation::Pose pose;
    uint64_t        revision = 0;
};
Snapshot snapshot(pack::ResourceId const& gun);

} // namespace tacz::client::anim
