#pragma once

#include "pack/GunData.h"

class Player;

// Server-driven bullets: each shot spawns a tacz:bullet entity (behavior pack) that the mod
// moves every server tick with TACZ's motion model, stopping at the first block it hits.
// Entity hits and damage are not wired up yet.
namespace tacz::server::bullet {

void enable();
void disable();

// Fires one bullet from the shooter's eyes along their view direction.
void fire(Player& shooter, pack::BulletData const& data);

} // namespace tacz::server::bullet
