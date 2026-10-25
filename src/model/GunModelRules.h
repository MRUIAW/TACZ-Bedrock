#pragma once

#include <numbers>
#include <string>
#include <string_view>

#include <glm/gtc/matrix_transform.hpp>

#include "model/GeoModel.h"

// Which gun-model bones TACZ draws, and where the model sits in first person.
// Mirrors BedrockGunModel's functional renderers and GunItemRendererWrapper.renderFirstPerson.
namespace tacz::model {

// Per-frame inputs to the bone visibility rules. Attachments are not supported yet,
// so every attachment slot is treated as empty.
struct GunRenderState {
    bool bulletInBarrel   = true;
    int  ammoCount        = 1;
    int  extendedMagLevel = 0;
    bool additionalMagazine = false;

    auto operator<=>(GunRenderState const&) const = default;
};

inline bool additionalMagazineKeyed(animation::Pose const& pose) {
    auto it=pose.find("additional_magazine");
    return it!=pose.end() && (it->second.translation || it->second.rotation || it->second.scale);
}

namespace detail {
inline bool isShellNode(std::string_view name) {
    if (name == "shell") return true;
    if (!name.starts_with("shell_") || name.size() == 6) return false;
    for (char c : name.substr(6)) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}
} // namespace detail

// Bones that are hidden, or that a functional renderer replaces (its own cubes are then
// never drawn), with no attachment installed.
inline Visit gunVisibility(Bone const& bone, Bone const* parent, GunRenderState const& state) {
    if (parent && parent->name == "attachment_adapter") return Visit::Hide;
    std::string_view n = bone.name;
    if (n.empty()) return Visit::Draw;
    // Drawn by LeftHandRender / RightHandRender / MuzzleFlashRender / ShellRender instead.
    if (n == "lefthand_pos" || n == "righthand_pos" || n == "muzzle_flash" || detail::isShellNode(n)) return Visit::Hide;
    // Only visible while an animation keyframes it (ModelAdditionalMagazineListener).
    if (n == "additional_magazine") return state.additionalMagazine ? Visit::Draw : Visit::Hide;
    if (n == "bullet_in_barrel") return state.bulletInBarrel ? Visit::Draw : Visit::Hide;
    if (n == "bullet_in_mag" || n == "bullet_chain") return state.ammoCount > 0 ? Visit::Draw : Visit::Hide;
    if (n == "mag_standard") return state.extendedMagLevel == 0 ? Visit::Draw : Visit::Hide;
    if (n == "mag_extended_1") return state.extendedMagLevel == 1 ? Visit::Draw : Visit::Hide;
    if (n == "mag_extended_2") return state.extendedMagLevel == 2 ? Visit::Draw : Visit::Hide;
    if (n == "mag_extended_3") return state.extendedMagLevel == 3 ? Visit::Draw : Visit::Hide;
    // No scope: rail and folded sight hidden, carry handle and iron sight shown.
    if (n == "mount" || n == "sight_folded") return Visit::Hide;
    // No laser or grip: default handguard shown, tactical hidden.
    if (n == "handguard_tactical") return Visit::Hide;
    // Attachment slots render the attachment model in place of the node.
    for (std::string_view type : {"muzzle", "stock", "grip", "laser", "extended_mag"}) {
        if (n.size() == type.size() + 4 && n.starts_with(type) && n.ends_with("_pos")) return Visit::Hide;
    }
    return Visit::Draw;
}

// View-space transform of the gun at rest, not aiming:
//   T(0, 1.5, 0) * Rz(180) * [T(0, 1.5, 0) * inverse(idle_view) * T(0, -1.5, 0)]
// The bracket is applyFirstPersonPositioningTransform with aim and refit progress at 0.
inline glm::mat4 firstPersonIdleTransform(GeoModel const& model) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), {0, 1.5f, 0});
    m           = glm::rotate(m, std::numbers::pi_v<float>, {0, 0, 1});
    m           = glm::translate(m, {0, 1.5f, 0});
    m           = m * positioningInverse(model, model.path(model.find("idle_view")));
    m           = glm::translate(m, {0, -1.5f, 0});
    return m;
}

} // namespace tacz::model
