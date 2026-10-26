#pragma once

#include <cstdint>

#include "gun/Reload.h"

class IClientInstance;

// Client side of gun operation, after TACZ's ShootKey / LocalPlayerShoot / LocalPlayerFireSelect
// / LocalPlayerReload: while a gun is held, the left mouse button is the trigger (vanilla attack
// is suppressed), G switches the fire mode and R reloads. Holding the trigger fires at the
// gun's rate in AUTO (and in BURST with continuous_shoot); SEMI and BURST need a new press per
// pull. The client plays its own sounds right away (shoot, dry fire, fire select, and the reload
// animation's sound keyframes via the Lua animation controller) and tells the server, which
// decides and reports the reload state. H inspects the held gun.
namespace tacz::client::shooter {

// Zero outside ClientJoinLevelEvent..ClientExitLevelEvent; changes on every rejoin.
uint64_t worldToken();
bool ownsClient(IClientInstance const* client); // cached identity only, no native service access

void enable();
void disable();

// The local player's reload state as last predicted or reported by the server.
gun::ReloadStateType reloadState();

struct AnimationTiming {
    int64_t interval = 0;
    int64_t cooldown = 0;
    int64_t lastShootTimestamp = -1;
    int fireMode = 3;
};
// Client frame thread only; timestamps returned to scripts use system milliseconds, as TACZ.
AnimationTiming animationTiming();
void adjustShootInterval(int64_t delta);

} // namespace tacz::client::shooter
