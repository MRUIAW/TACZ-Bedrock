#pragma once
#include <string>
class IClientInstance;
namespace tacz::client::hud {
void enable();
void disable();
void releaseWorld(IClientInstance const* client); // only cached owners, no service lookup
void rememberTooltip(std::string text,std::string icon);
} // namespace tacz::client::hud
