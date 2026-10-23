#pragma once

// Server-side gun operation for players, after TACZ's LivingEntityShoot / LivingEntityFireSelect
// / LivingEntityReload and ModernKineticGunScriptAPI.shootOnce:
// - trigger packets are checked against the fire-rate cooldown, the client clock, reloading and
//   the gun's ammo; each round consumes ammo and spawns bullets (bursts over server ticks);
// - fire-select packets cycle the fire mode;
// - reload packets start a reload that the gun's Lua script (or TACZ's default logic) advances
//   every server tick; the state is sent to the client on every change. Ammo comes from items
//   carrying TACZ's AmmoId NBT; creative players reload for free.
//
// Not yet: bolt action, draw/sprint/melee locks, heat, Lua `shoot` overrides, attachments
// (extended magazines), inventory-fed ammo, third-person sounds for other players.
namespace tacz::server::shooter {

void enable();
void disable();

} // namespace tacz::server::shooter
