#pragma once
#include <cstdint>
#include "model/GeoModel.h"

namespace tacz::model {
// An ordered draw run in one named bone's local frame. Only anonymous, static cube
// helper descendants are folded in. Named bones remain independent even if today's
// clips never animate them: future Lua-selected clips cannot invalidate grouping.
struct RigidGunGroup {
    int bone=-1;
    bool illuminated=false;
    std::vector<Vertex> vertices;
};
struct RigidGunModel {
    std::vector<glm::mat4> restLocal;
    std::vector<int> traversal;
    std::vector<RigidGunGroup> groups;
};
struct BoneFrame {
    std::vector<glm::mat4> world;
    std::vector<uint8_t> visible, parentVisible;
};
RigidGunModel compileRigidGunModel(GeoModel const& model);
// Anonymous helpers have no authored name. An explicitly empty-name animation is
// not eligible for static folding; caller can safely use the retained baked path.
bool rigidGunPoseCompatible(animation::Pose const& pose);
bool drawableRigidBone(BoneFrame const& frame,int bone);
void evaluateBoneFrame(GeoModel const& model,RigidGunModel const& rigid,
    animation::Pose const& pose,VisitFn const& visit,BoneFrame& frame);
// CPU reference/diagnostic only. Production path 1 submits local vertices unchanged.
std::vector<Vertex> flattenRigidGunModel(RigidGunModel const& rigid,BoneFrame const& frame);
} // namespace tacz::model
