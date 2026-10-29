#include "model/ViewTurnInertia.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include "model/FunctionalBones.h"

namespace tacz::model {
double shortestTurn(double from, double to) {
    // Reduce separately: subtraction of two large finite inputs must not overflow.
    double d=std::fmod(std::remainder(to,360.)-std::remainder(from,360.)+540.,360.)-180.;
    return d;
}
void ViewTurnBob::initialize(double x, double y) {
    *this={};
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    initialized=true;
    pitch=xBob=xBobO=x;
    yaw=yBob=yBobO=lastYaw=std::remainder(y,360.);
    // Deliberate 2a divergence: Java starts bob fields at zero; world/player/respawn
    // initialization here starts at the current view to avoid an opening sway.
}
void ViewTurnBob::tick(double x, double y, bool advance) {
    if (!std::isfinite(x) || !std::isfinite(y)) { *this={}; return; }
    if (!initialized) { initialize(x,y); return; }
    pitch=x;
    yaw+=shortestTurn(lastYaw,y);
    lastYaw=std::remainder(y,360.);
    // Rebase view AND both bob samples together; never wrap view-minus-bob.
    double shift=std::round(yaw/360.)*360.;
    yaw-=shift; yBob-=shift; yBobO-=shift;
    if (!advance) return;
    xBobO=xBob; yBobO=yBob; // Java copies before its fixed half-step.
    xBob+=(pitch-xBob)*.5;
    yBob+=(yaw-yBob)*.5;
}
ViewTurnFrame ViewTurnBob::frame(double x, double y, float alpha) const {
    if (!initialized || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(alpha)) return {};
    double a=std::clamp(alpha,0.f,1.f);
    return viewTurnFrame(x-std::lerp(xBobO,xBob,a),yaw+shortestTurn(lastYaw,y)-std::lerp(yBobO,yBob,a));
}
ViewTurnFrame viewTurnFrame(double dx, double dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy)) return {};
    return {dx,dy,static_cast<float>(std::tanh(dx/25.))*25.f,static_cast<float>(std::tanh(dy/25.))*25.f};
}
glm::mat4 viewTurnResidual(ViewTurnFrame const& turn) {
    // RAW offsets, not the tanh-limited root offsets. Positive right-handed axes,
    // postmultiplication like Java PoseStack / Axis.XP and Axis.YP.
    auto a=glm::radians(std::remainder(turn.dx*.1,360.));
    auto b=glm::radians(std::remainder(turn.dy*.1,360.));
    auto c=glm::rotate(glm::dmat4(1),a,{1,0,0});
    c=glm::rotate(c,b,{0,1,0});
    c=glm::rotate(c,-a,{1,0,0});
    return glm::mat4(glm::rotate(c,-b,{0,1,0}));
}
animation::Pose effectiveViewTurnPose(GeoModel const& model, animation::Pose const& animation, ViewTurnFrame const& turn) {
    auto result=animation;
    if (model.find("root")<0 || (turn.sx==0 && turn.sy==0)) return result;
    auto& root=result["root"];
    // BonePose has animation +Y; partTransform converts it to BedrockPart's -Y.
    root.translation=root.translation.value_or(glm::vec3(0))+glm::vec3(turn.sy*.1f/48.f,turn.sx*.1f/48.f,0);
    auto q=glm::quat(root.rotation.value_or(glm::vec3(0))); // additional Rz*Ry*Rx
    q=q*glm::angleAxis(glm::radians(turn.sx*.05f),glm::vec3(1,0,0))
        *glm::angleAxis(glm::radians(turn.sy*.05f),glm::vec3(0,1,0));
    root.rotation=quaternionAngles(glm::normalize(q));
    return result; // immutable animation/rest data; no accumulation or third-person leak
}
bool HandTurnCaptureKey::ready() const {
    return scope && world && client && player && camera && stack && top && depth;
}
bool validHandTurnCapture(HandTurnCaptureKey const& captured, HandTurnCaptureKey const& current) {
    return captured.ready() && current.ready() && captured==current;
}
} // namespace tacz::model
