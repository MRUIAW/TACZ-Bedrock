#pragma once
class ClientLevel;
namespace tacz::client::bullet {
void enable();
void disable();
void clear();
void tick(ClientLevel& level);
}
