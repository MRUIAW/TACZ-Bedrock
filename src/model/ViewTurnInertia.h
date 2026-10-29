#pragma once

#include <cstdint>
#include "model/GeoModel.h"

namespace tacz::model {

// Degrees throughout; only the root animation quaternion uses radians.
double shortestTurn(double from, double to);
struct ViewTurnFrame {
    double dx=0, dy=0;
    float sx=0, sy=0;
    bool operator==(ViewTurnFrame const&) const = default;
};
struct ViewTurnBob {
    bool initialized=false;
    double pitch=0, yaw=0, xBob=0, xBobO=0, yBob=0, yBobO=0;
    double lastYaw=0;
    void initialize(double x, double y);
    // Observe yaw even when frozen, but never advance/copy bob while immobile/off-camera.
    void tick(double x, double y, bool advance=true);
    ViewTurnFrame frame(double x, double y, float alpha) const;
};
ViewTurnFrame viewTurnFrame(double dx, double dy);
glm::mat4 viewTurnResidual(ViewTurnFrame const& turn);
float viewTurnResidualDegrees(glm::mat4 const& residual);
animation::Pose effectiveViewTurnPose(GeoModel const& model, animation::Pose const& animation, ViewTurnFrame const& turn);

// Comparison-only identities. Native objects are never dereferenced by the pure validator.
struct HandTurnCaptureKey {
    uint64_t scope=0, world=0;
    uintptr_t client=0, player=0, camera=0, stack=0, top=0;
    size_t depth=0;
    bool operator==(HandTurnCaptureKey const&) const = default;
    bool ready() const;
};
bool validHandTurnCapture(HandTurnCaptureKey const& captured, HandTurnCaptureKey const& current);
} // namespace tacz::model
