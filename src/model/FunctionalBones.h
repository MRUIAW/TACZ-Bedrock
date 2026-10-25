#pragma once

#include <glm/gtc/quaternion.hpp>
#include "model/GeoModel.h"

namespace tacz::model {
glm::vec3 quaternionAngles(glm::quat q);
glm::quat cameraBoneRotation(glm::vec3 radians);
glm::quat blendCameraRotation(glm::quat a, glm::quat b);
float positiveDegrees(float radians);
glm::quat scaledCameraRotation(glm::quat q, float aim, float zoom);
glm::mat4 applyCameraAngles(glm::mat4 const& view, glm::quat rotation);
glm::mat4 modelProjectionTransform(glm::mat4 const& nativeProjection, float fov, int depthMode = 0);
// FirstPersonRenderGunEvent: animation before rest rotation, no scale, and the
// constraint node's own position/rotation channels are coefficients, NOT transforms.
struct ConstraintTransform {
    glm::vec3 rest{0}, animated{0}, rotation{0};
    glm::vec3 translationCoefficient{0}, rotationCoefficient{0};
    bool present = false;
};
ConstraintTransform constraintTransform(GeoModel const& model, animation::Pose const& pose);
animation::Pose functionalInitialPose(GeoModel const& model);
glm::mat4 applyConstraint(glm::mat4 matrix, ConstraintTransform const& constraint, float aim);
// Project a point along the iron_view optical axis (the origin itself is the eye,
// w=0 at full ADS and cannot be projected). Returned coordinates are NDC.
glm::vec3 sightProjection(GeoModel const& model, animation::Pose const& pose,
    glm::mat4 const& viewModel, glm::mat4 const& projection);
} // namespace tacz::model
