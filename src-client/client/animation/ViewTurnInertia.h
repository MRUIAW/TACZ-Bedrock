#pragma once
#include "model/ViewTurnInertia.h"
class IClientInstance;
class LocalPlayer;
namespace tacz::client::turn {
struct Sample {
    model::ViewTurnFrame frame;
    double tickPitch=0, tickYaw=0;
};
void tick();
void clear();
Sample sample(IClientInstance& client, LocalPlayer& player, float alpha);
} // namespace tacz::client::turn
