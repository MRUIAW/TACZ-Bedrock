#pragma once

// Server-side gun operation for players, after TACZ's LivingEntityShoot / LivingEntityFireSelect
// and ModernKineticGunScriptAPI.shootOnce: trigger packets are checked against the fire-rate
// cooldown, the client clock and the gun's ammo, then each round consumes ammo and spawns
// bullets (bursts are spread over server ticks). Fire-select packets cycle the fire mode.
//
// Not yet: reload, bolt action, draw/sprint/melee locks, heat, Lua `shoot` overrides,
// inventory-fed ammo, third-person shoot sounds for other players.
namespace tacz::server::shooter {

void enable();
void disable();

} // namespace tacz::server::shooter
