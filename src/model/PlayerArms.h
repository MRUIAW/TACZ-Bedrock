#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "model/GeoModel.h"

namespace tacz::model {

enum class Arm { Left, Right };

// Vanilla PlayerModel + PlayerRenderer.renderHand after TACZ's PlayerModelMixin clears
// rotations. setupAnim(crouching=false) sets BOTH wide/slim part pivots to (+/-5, 2, 0).
std::vector<Vertex> playerArmVertices(Arm side, bool slim, bool sleeve);

// Cached root-to-functional-node path; the gun's view/world matrix is applied by the caller.
// Own functional-node cubes/visibility are replaced, but hidden ancestors still hide the arm.
std::optional<glm::mat4> playerArmTransform(GeoModel const& model, std::span<int const> path,
    animation::Pose const& pose);

bool isClassicArmSkin(uint32_t width, uint32_t height, size_t bytes, bool persona,
    bool animated, std::string_view geometry);

// GPU textures need neither RGBA CPU bytes nor access to SerializedSkinImpl. Dimensions
// alone do NOT prove a Persona atlas has classic UVs: require a known renderer geometry.
std::optional<bool> renderArmSkinLayout(uint32_t width, uint32_t height, std::string_view geometry);

// Java uses entitySolid for the base arm. Preserve sleeve alpha, force only base-arm texels
// opaque so our alpha-test base pass has the same treatment, without changing the player's skin.
void makeArmSkinBaseOpaque(std::span<uint8_t> rgba, uint32_t width, uint32_t height);
uint64_t armSkinHash(std::span<uint8_t const> rgba);

} // namespace tacz::model
