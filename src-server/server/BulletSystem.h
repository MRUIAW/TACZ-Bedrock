#pragma once

#include "pack/GunData.h"

class Player;

// Server-driven bullets: each shot spawns a tacz:bullet entity (behavior pack) that the mod
// moves every server tick with TACZ's motion model. Per tick the path is ray-traced against
// blocks and entities; entity hits deal TACZ damage (distance falloff, headshot, armour
// ignore, pierce). Not yet: player latency rewind, explosions, water drag, hit feedback UI.
namespace tacz::server::bullet {

void enable();
void disable();

// Fires one round from the shooter's eyes along their view direction (bullet_amount pellets).
void fire(Player& shooter, pack::GunData const& gun, pack::FireMode mode);

} // namespace tacz::server::bullet
