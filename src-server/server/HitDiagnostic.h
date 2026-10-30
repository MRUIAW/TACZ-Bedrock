#pragma once

#include <string>
#include "debug/HitDiagnostic.h"

class Actor;
class Player;
class Level;

namespace tacz::server::hitdiag {
void arm(Player& player, unsigned shots);
void clear();
void tick(Level& level);
debug::hit::Tag trigger(Player& player, int64_t timestamp);
void spawn(Player& shooter, debug::hit::Tag tag, int serial, int pellet, glm::dvec3 origin,
    glm::dvec3 direction, glm::dvec3 velocity);
void segment(debug::hit::Tag tag, int serial, int age, glm::dvec3 from, glm::dvec3 end,
    glm::dvec3 uncutEnd, bool blockHit);
void candidate(debug::hit::Tag tag, int serial, int age, Actor& actor, gun::HitboxTarget const& target,
    glm::dvec3 from, glm::dvec3 end);
} // namespace tacz::server::hitdiag
