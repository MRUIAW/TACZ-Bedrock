#include "model/PlayerArms.h"

#include <cmath>
#include <numbers>

#include "model/GunModelRules.h"

namespace tacz::model {

std::vector<Vertex> playerArmVertices(Arm side, bool slim, bool sleeve) {
    bool left = side == Arm::Left;
    float width = slim ? 3.0f : 4.0f;
    // Includes the vanilla part pivot, NOT a guessed shoulder-centred box. Modern left-arm
    // UVs have their own island: PlayerModel replaces the mirrored HumanoidModel left arm.
    glm::vec3 origin{left ? 4.0f : (slim ? -7.0f : -8.0f), 0, -2};
    glm::vec2 uv = left ? (sleeve ? glm::vec2{48, 48} : glm::vec2{32, 48})
                        : (sleeve ? glm::vec2{40, 32} : glm::vec2{40, 16});
    return boxVertices(origin, {width, 12, 4}, uv, {64, 64}, sleeve ? 0.25f : 0.0f);
}

std::optional<glm::mat4> playerArmTransform(GeoModel const& model, std::span<int const> path,
    animation::Pose const& pose) {
    if (path.empty()) return std::nullopt;
    glm::mat4 result(1);
    GunRenderState state;
    for (size_t i = 0; i < path.size(); ++i) {
        auto index = path[i];
        if (index < 0 || static_cast<size_t>(index) >= model.bones.size()) return std::nullopt;
        auto const& bone = model.bones[index];
        if (i && bone.parent != path[i - 1]) return std::nullopt;
        if (!i && bone.parent != -1) return std::nullopt;
        if (i + 1 != path.size() && gunVisibility(bone,
            bone.parent < 0 ? nullptr : &model.bones[bone.parent], state) == Visit::Hide) return std::nullopt;
        auto p = pose.find(bone.name);
        result *= partTransform(bone, p == pose.end() ? nullptr : &p->second);
    }
    // LeftHandRender and RightHandRender BOTH append exactly this Z rotation. No extra
    // negative scale, guessed 0.9375 player-body scale, or independent hand sway.
    result = glm::rotate(result, std::numbers::pi_v<float>, glm::vec3{0, 0, 1});
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        if (!std::isfinite(result[c][r])) return std::nullopt;
    if (std::abs(glm::determinant(glm::mat3(result))) < 1e-12f) return std::nullopt;
    return result;
}

bool isClassicArmSkin(uint32_t width, uint32_t height, size_t bytes, bool persona,
    bool animated, std::string_view geometry) {
    return !persona && !animated && renderArmSkinLayout(width, height, geometry).has_value()
        && bytes == static_cast<size_t>(width) * height * 4;
}

std::optional<bool> renderArmSkinLayout(uint32_t width, uint32_t height, std::string_view geometry) {
    if ((width != 64 && width != 128) || height != width) return std::nullopt;
    // These two identifiers/layouts are verified in the installed vanilla geometry.json.
    // Do not admit unverified aliases or a square Persona atlas by dimensions alone.
    if (geometry == "geometry.humanoid.customSlim") return true;
    if (geometry == "geometry.humanoid.custom") return false;
    return std::nullopt;
}

void makeArmSkinBaseOpaque(std::span<uint8_t> rgba, uint32_t width, uint32_t height) {
    if ((width != 64 && width != 128) || height != width || rgba.size() != size_t(width) * height * 4) return;
    unsigned scale = width / 64;
    for (auto uv : {glm::uvec2{40, 16}, glm::uvec2{32, 48}}) {
        for (unsigned y = uv.y * scale; y < (uv.y + 16) * scale; ++y)
            for (unsigned x = uv.x * scale; x < (uv.x + 16) * scale; ++x)
                rgba[(size_t(y) * width + x) * 4 + 3] = 255;
    }
}

uint64_t armSkinHash(std::span<uint8_t const> rgba) {
    uint64_t hash = 14695981039346656037ull;
    for (auto byte : rgba) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

} // namespace tacz::model
