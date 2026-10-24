#pragma once

#include <array>
#include "model/GeoModel.h"

class IClientInstance;
class LocalPlayer;
class ScreenContext;

namespace tacz::client::render {

using ArmPaths = std::array<std::vector<int>, 2>; // left, right

// Called under GunRenderer's cache lock, only after its world/resource-key checks. The
// current world matrix already contains the gun's first-person positioning transform.
void drawPlayerArms(ScreenContext& screen, IClientInstance& client, LocalPlayer& player,
    model::GeoModel const& geo, ArmPaths const& paths, animation::Pose const& pose,
    uint64_t world, uint64_t resourceSerial, bool rtx);
void releasePlayerArms(char const* reason); // existing owners only; never queries globals

} // namespace tacz::client::render
