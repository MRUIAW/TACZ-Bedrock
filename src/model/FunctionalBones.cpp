#include "model/FunctionalBones.h"

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include "model/Aiming.h"

namespace tacz::model {
glm::vec3 quaternionAngles(glm::quat q) {
    return {std::atan2(2*(q.w*q.x+q.y*q.z), 1-2*(q.x*q.x+q.y*q.y)),
        std::asin(std::clamp(2*(q.w*q.y-q.x*q.z), -1.f, 1.f)),
        std::atan2(2*(q.w*q.z+q.x*q.y), 1-2*(q.y*q.y+q.z*q.z))};
}
glm::quat cameraBoneRotation(glm::vec3 r) { return glm::quat(glm::vec3(r.x, r.y, -r.z)); }
glm::quat blendCameraRotation(glm::quat a, glm::quat b) {
    // MathUtil.blendQuaternion = exp(log(a)+log(b)), not a*b or Euler addition.
    auto log = [](glm::quat q) {
        q = glm::normalize(q);
        auto s = glm::length(glm::vec3(q.x,q.y,q.z));
        return s > 1e-7f ? glm::vec3(q.x,q.y,q.z) * (std::acos(std::clamp(q.w,-1.f,1.f))/s) : glm::vec3(0);
    };
    auto v = log(a) + log(b);
    auto n = glm::length(v);
    return n > 1e-7f ? glm::quat(std::cos(n), v * (std::sin(n)/n)) : glm::quat(1,0,0,0);
}
float positiveDegrees(float r) {
    constexpr float tau = 6.283185307179586f;
    if (r < 0) r += std::ceil(-r/tau)*tau;
    return glm::degrees(r);
}
glm::quat scaledCameraRotation(glm::quat q, float aim, float zoom) {
    if (!std::isfinite(zoom) || zoom <= 0) zoom = 1;
    aim = std::clamp(aim,0.f,1.f);
    float multiplier = 1-aim+aim/std::sqrt(zoom); // unlike recoil, NO sqrt cap at 1.5
    q = glm::normalize(q);
    float angle = std::acos(std::clamp(q.w,-1.f,1.f));
    float s = std::sin(angle);
    return std::abs(s) > 1e-7f ? glm::quat(std::cos(angle*multiplier), glm::vec3(q.x,q.y,q.z)*(std::sin(angle*multiplier)/s))
                             : glm::quat(1,0,0,0);
}
glm::mat4 applyCameraAngles(glm::mat4 const& view, glm::quat q) {
    // Java GameRenderer builds Rz(roll)*Rx(pitch)*Ry(yaw+180). Add the camera
    // event angles, retaining the eye position and native post-view translations.
    float roll, pitch, yaw;
    glm::extractEulerAngleZXY(view,roll,pitch,yaw);
    auto delta = quaternionAngles(q);
    auto before = glm::eulerAngleZXY(roll,pitch,yaw);
    auto after = glm::eulerAngleZXY(roll+delta.z,pitch+delta.x,yaw+delta.y);
    return after*glm::transpose(before)*view;
}
glm::quat cameraEntityOrientation(glm::quat original, glm::quat animation) {
    if (animation.x==0 && animation.y==0 && animation.z==0) return original;
    auto view = glm::transpose(glm::mat4_cast(glm::normalize(original)));
    auto result = glm::normalize(glm::quat_cast(glm::transpose(glm::mat3(applyCameraAngles(view,animation)))));
    return glm::dot(result,original) < 0 ? -result : result;
}
glm::mat4 modelProjectionTransform(glm::mat4 const& native, float fov, int mode) {
    if (!projectionFov(native) || std::abs(glm::determinant(native)) < 1e-12f) return glm::mat4(1);
    auto desired = withModelFov(native,fov);
    if (mode == 1 || mode == 2) {
        // Preserve Bedrock's [0,1] depth convention inferred from the native matrix:
        // native near .05, p22~-1,p32~-.05. No shared depth buffer is cleared here.
        float nearPlane = mode == 2 ? .1f : .05f, farPlane = 16.f;
        desired[2][2] = farPlane/(nearPlane-farPlane);
        desired[3][2] = farPlane*nearPlane/(nearPlane-farPlane);
    }
    // P_native * correction == P_model. Put this projective correction in OUR
    // model transform rather than assuming a late Camera projection-stack edit
    // reaches RenderDragon. Per-draw transforms are user-verified in RTX. No double FOV.
    return glm::inverse(native)*desired;
}
namespace {
glm::mat4 restRotation(glm::mat4 m, glm::vec3 r) {
    m = glm::rotate(m,r.z,{0,0,1});
    m = glm::rotate(m,r.y,{0,1,0});
    return glm::rotate(m,r.x,{1,0,0});
}
// MathUtil.getEulerAngles(Matrix4f): JOML's getEulerAnglesZYX.
glm::vec3 matrixAngles(glm::mat4 const& m) {
    return {std::atan2(m[1][2],m[2][2]), std::atan2(-m[0][2],std::sqrt(m[1][2]*m[1][2]+m[2][2]*m[2][2])),
        std::atan2(m[0][1],m[0][0])};
}
}
ConstraintTransform constraintTransform(GeoModel const& model, animation::Pose const& pose) {
    ConstraintTransform result;
    auto node = model.find("constraint");
    if (node < 0) return result;
    result.present = true;
    glm::mat4 rest(1), animated(1);
    for (int i : model.path(node)) {
        auto const& bone = model.bones[i];
        auto found = pose.find(bone.name);
        auto p = found != pose.end() ? &found->second : nullptr;
        if (i != node && p && p->translation) animated = glm::translate(animated,*p->translation*glm::vec3(1,-1,1));
        auto pivot = bone.pivot/16.f;
        if (bone.parent < 0) pivot.y -= 1.5f;
        animated = glm::translate(animated,pivot);
        rest = glm::translate(rest,pivot);
        if (i != node && p && p->rotation) animated *= glm::mat4_cast(glm::quat(*p->rotation));
        animated = restRotation(animated,bone.rotation);
        rest = restRotation(rest,bone.rotation);
    }
    result.rest = rest[3]; result.animated = animated[3];
    result.rotation = matrixAngles(animated)-matrixAngles(rest);
    if (auto it = pose.find("constraint"); it != pose.end()) {
        if (it->second.translation) result.translationCoefficient = *it->second.translation*16.f;
        if (it->second.rotation) for (int i=0;i<3;++i) result.rotationCoefficient[i] = positiveDegrees((*it->second.rotation)[i]);
    }
    return result;
}
animation::Pose functionalInitialPose(GeoModel const& model) {
    animation::Pose pose;
    if (int i = model.find("constraint"); i >= 0) {
        auto const& bone = model.bones[i];
        auto pivot = bone.pivot;
        if (bone.parent < 0) pivot.y -= 24; // root listener: (rawX,-rawY,rawZ)
        pose["constraint"].translation = pivot/16.f;
        pose["constraint"].rotation = glm::vec3(0);
    }
    pose["camera"].rotation = glm::vec3(0);
    return pose;
}
glm::mat4 applyConstraint(glm::mat4 m, ConstraintTransform const& c, float weight) {
    if (!c.present || weight == 0) return m;
    auto inverse = glm::mat3(m)*(c.rest-c.animated);
    inverse *= glm::vec3(c.translationCoefficient.x-1,c.translationCoefficient.y-1,1-c.translationCoefficient.z);
    auto r = c.rotation*(c.rotationCoefficient-glm::vec3(1))*weight;
    auto pivot = c.animated+glm::vec3(0,1.5f,0);
    m = glm::translate(m,pivot);
    m = glm::rotate(m,r.x,{1,0,0});
    m = glm::rotate(m,r.y,{0,1,0});
    m = glm::rotate(m,r.z,{0,0,1});
    m = glm::translate(m,-pivot);
    m[3] += glm::vec4(-inverse.x,-inverse.y,inverse.z,0)*weight;
    return m;
}
glm::vec3 sightProjection(GeoModel const& model, animation::Pose const& pose, glm::mat4 const& mv, glm::mat4 const& projection) {
    glm::mat4 path(1);
    auto node = model.find("iron_view");
    if (node < 0) return glm::vec3(0);
    for (int i : model.path(node)) {
        auto it = pose.find(model.bones[i].name);
        path *= partTransform(model.bones[i],it == pose.end() ? nullptr : &it->second);
    }
    auto point = projection*mv*path*glm::vec4(0,0,-1,1);
    return std::abs(point.w)>1e-7f ? glm::vec3(point)/point.w : glm::vec3(0);
}
} // namespace tacz::model
