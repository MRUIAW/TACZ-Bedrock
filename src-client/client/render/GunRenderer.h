#pragma once

// First-person gun rendering: while the local player holds a gun, the vanilla first-person
// pass is replaced with the gun's TACZ model and texture, read straight from the gun pack.
// Animated CPU-baked meshes; GPU resources belong to a world/pipeline, not the pack cache.
class IClientInstance;
namespace tacz::client::render {

void enable();
void disable();
void releaseWorld(IClientInstance const* client); // identity only; never dereferenced
void diagnoseFrame(); // low-rate heartbeat independent of the first-person render hook

} // namespace tacz::client::render
