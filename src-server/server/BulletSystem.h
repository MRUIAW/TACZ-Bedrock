#pragma once

#include <optional>
#include <string>

#include "pack/GunData.h"
#include "debug/HitDiagnostic.h"
#include "gun/ShooterInaccuracy.h"

class Player;

// Server-driven bullets: each shot spawns a tacz:bullet entity (behavior pack) that the mod
// simulates every server tick and places every five ticks (plus spawn/final endpoint).
// Per tick the authoritative path is ray-traced against
// blocks and entities; entity hits deal TACZ damage (distance falloff, headshot, armour
// ignore, pierce). Accepted damage sends hit/kill feedback to the shooter.
// Not yet: player latency rewind, explosions.
namespace tacz::server::bullet {

void enable();
void disable();
std::string experimentStatus();
void armDiagnostic(Player& player, unsigned bullets);

// Fires one round (bullet_amount pellets) from the shooter's eyes along their view direction,
// each pellet with TACZ's spread for `inaccuracy.value`.
// inaccuracy retains its preparation-time input/type/count for diagnostics; its
// movement is not resampled here or added to projectile velocity.
void fire(Player& shooter,pack::ResourceId const& gunId, pack::GunData const& gun, std::optional<pack::FireMode> mode,
    gun::RoundInaccuracy const& inaccuracy, debug::hit::Tag diagnostic = {});

} // namespace tacz::server::bullet
