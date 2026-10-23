#pragma once

#include "gun/Reload.h"

// Client side of gun operation, after TACZ's ShootKey / LocalPlayerShoot / LocalPlayerFireSelect
// / LocalPlayerReload: while a gun is held, the left mouse button is the trigger (vanilla attack
// is suppressed), G switches the fire mode and R reloads. Holding the trigger fires at the
// gun's rate in AUTO (and in BURST with continuous_shoot); SEMI and BURST need a new press per
// pull. The client plays its own sounds right away (shoot, dry fire, fire select, and the reload
// animation's sound keyframes) and tells the server, which decides and reports the reload state.
namespace tacz::client::shooter {

void enable();
void disable();

// The local player's reload state as last predicted or reported by the server.
gun::ReloadStateType reloadState();

} // namespace tacz::client::shooter
