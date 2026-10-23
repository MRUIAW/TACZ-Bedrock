#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "pack/ResourceId.h"

// assets/<ns>/display/guns/*.json (TACZ client/resource/pojo/display/gun/GunDisplay).
// Only the fields the renderer uses so far; the rest stays in `raw`.
namespace tacz::pack {

struct GunDisplay {
    std::optional<ResourceId> model;      // geo_models/<path>.json
    std::optional<ResourceId> texture;    // textures/<path>.png
    std::optional<ResourceId> animation;  // animations/<path>.animation.json
    std::optional<ResourceId> stateMachine;
    std::string               useDefaultAnimation;
    float                     ironZoom           = 1.2f;
    float                     zoomModelFov       = 70.0f;
    bool                      enableTransparency = false;
    nlohmann::json            raw;
};

// "assets/<ns>/<dir>/<path><ext>", the file a resource id points at.
inline std::string assetPath(ResourceId const& id, std::string_view dir, std::string_view ext) {
    return "assets/" + id.ns + "/" + std::string(dir) + "/" + id.path + std::string(ext);
}

} // namespace tacz::pack
