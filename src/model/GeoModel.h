#pragma once

#include <array>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "model/Animation.h"

// Bedrock geometry loaded the way TACZ loads it (client/model/bedrock/BedrockModel and
// BedrockCubeBox/BedrockCubePerFace), so the same pack models render identically:
// positions are in Java ModelPart convention (pixels, y down) and the whole model is
// flipped by the first-person transform later.
namespace tacz::model {

class ModelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Quad {
    std::array<glm::vec3, 4> pos; // pixels, relative to the owning bone's pivot
    std::array<glm::vec2, 4> uv;  // normalised by the geometry's texture size
    glm::vec3                normal;
};

struct Bone {
    std::string      name;         // empty for the helper part TACZ creates for a rotated cube
    int              parent = -1;  // index into GeoModel::bones
    glm::vec3        pivot{0};     // BedrockPart.x/y/z: pixels, relative to the parent, y flipped
    glm::vec3        rotation{0};  // BedrockPart.xRot/yRot/zRot in radians
    bool             illuminated = false;
    std::vector<Quad> quads;
    std::vector<int> children;
};

struct GeoModel {
    std::string                          identifier;
    int                                  textureWidth  = 0;
    int                                  textureHeight = 0;
    std::vector<Bone>                    bones;
    std::vector<int>                     roots; // render order
    std::unordered_map<std::string, int> byName;

    [[nodiscard]] int find(std::string const& name) const {
        auto it = byName.find(name);
        return it == byName.end() ? -1 : it->second;
    }

    // Root first, ending at `bone` (BedrockModel.getPath). Empty if bone < 0.
    [[nodiscard]] std::vector<int> path(int bone) const;
};

// Parses a geometry file (format_version 1.12.0 or newer). Throws ModelError.
GeoModel parseGeoModel(nlohmann::json const& json);

// BedrockPart.translateAndRotateAndScale at rest: T(pivot / 16) * Rz * Ry * Rx.
glm::mat4 partTransform(Bone const& bone, animation::BonePose const* pose = nullptr);

// FirstPersonRenderGunEvent.getPositioningNodeInverse: moves the end of `path` onto the
// render origin. Empty path gives identity.
glm::mat4 positioningInverse(GeoModel const& model, std::vector<int> const& path);

struct Vertex {
    glm::vec3 pos; // model space, blocks
    glm::vec2 uv;
    glm::vec3 normal;
};

// Java ModelPart box convention: pixel origin, dimensions and UV offsets; no pivot.
// Used for the neutral vanilla player arm, not a Bedrock JSON coordinate conversion.
std::vector<Vertex> boxVertices(glm::vec3 origin, glm::vec3 size, glm::vec2 uv,
    glm::vec2 textureSize, float inflate = 0, bool mirror = false);

enum class Visit { Draw, Hide };

// Decides per bone whether it (and its subtree) is drawn. `parent` is null for roots.
using VisitFn = std::function<Visit(Bone const& bone, Bone const* parent)>;

// Bakes the rest pose into vertices, 4 per quad, in the order BedrockModel.render emits them.
std::vector<Vertex> bake(GeoModel const& model, VisitFn const& visit, animation::Pose const& pose = {});

} // namespace tacz::model
