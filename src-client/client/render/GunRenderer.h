#pragma once

// First-person gun rendering: while the local player holds a gun, the vanilla first-person
// pass is replaced with the gun's TACZ model and texture, read straight from the gun pack.
// Rest pose only for now (no animation, no arms).
namespace tacz::client::render {

void enable();
void disable();

} // namespace tacz::client::render
