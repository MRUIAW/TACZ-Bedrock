#pragma once

#include <cstdint>
#include "pack/GunData.h"

class IClientInstance;
class LocalPlayer;

namespace tacz::client::recoil {
void enable();
void disable();
void clear(); // client frame thread, no engine/service queries or return-to-origin
void requestClear(); // any thread; frame consumes it
void shot(pack::GunRecoil const& data, pack::ResourceId const& gun, int64_t triggerTimestamp, bool burstRound = false);
void update(IClientInstance& client, LocalPlayer& player);
bool isApplyingTurn(LocalPlayer const& player); // scoped TLS, bypass only our mouse sensitivity
} // namespace tacz::client::recoil
