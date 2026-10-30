#pragma once
#include "gun/HitInput.h"
class Player;
namespace tacz::server::hitinput {
void enable();
void disable();
gun::AuthInputRing snapshot(Player& player);
} // namespace tacz::server::hitinput
