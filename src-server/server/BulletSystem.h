#pragma once

#include <optional>
#include <glm/glm.hpp>

#include "pack/GunData.h"
#include "debug/HitDiagnostic.h"

class Player;

// Server-driven bullets: each shot spawns a tacz:bullet entity (behavior pack) that the mod
// moves every server tick with TACZ's motion model. Per tick the path is ray-traced against
// blocks and entities; entity hits deal TACZ damage (distance falloff, headshot, armour
// ignore, pierce). Accepted damage sends hit/kill feedback to the shooter.
// Not yet: player latency rewind, explosions, water drag.
namespace tacz::server::bullet {

void enable();
void disable();

// Fires one round (bullet_amount pellets) from the shooter's eyes along their view direction,
// each pellet with TACZ's spread for `inaccuracy`.
// inaccuracyMovement is the input captured when selecting this round's inaccuracy,
// passed through for diagnostics; it is not resampled here or added to velocity.
void fire(Player& shooter,pack::ResourceId const& gunId, pack::GunData const& gun, std::optional<pack::FireMode> mode,
    float inaccuracy, glm::dvec3 inaccuracyMovement, debug::hit::Tag diagnostic = {});

} // namespace tacz::server::bullet
