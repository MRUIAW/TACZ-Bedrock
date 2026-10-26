#pragma once
class IClientInstance;
namespace tacz::client::hud {
void enable();
void disable();
void releaseWorld(IClientInstance const* client); // only cached owners, no service lookup
} // namespace tacz::client::hud
