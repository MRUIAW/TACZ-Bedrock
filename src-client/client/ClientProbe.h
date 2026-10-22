#pragma once

// Technical-validation probe, client side: sends actions to the server on
// key/mouse input, swallows vanilla left-click while holding a gun, and draws
// a debug HUD line with the held gun's NBT and the last server reply.
namespace tacz::client::probe {

void enable();
void disable();

} // namespace tacz::client::probe
