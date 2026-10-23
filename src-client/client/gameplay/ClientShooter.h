#pragma once

// Client side of shooting, after TACZ's ShootKey / LocalPlayerShoot / LocalPlayerFireSelect:
// while a gun is held, the left mouse button is the trigger (vanilla attack is suppressed) and
// G switches the fire mode. Holding the trigger fires at the gun's rate in AUTO (and in BURST
// with continuous_shoot); SEMI and BURST need a new press per pull. The client plays its own
// shoot / dry-fire / fire-select sounds right away and tells the server, which decides.
namespace tacz::client::shooter {

void enable();
void disable();

} // namespace tacz::client::shooter
