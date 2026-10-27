#pragma once
#include <cmath>
#include <optional>
#include <string_view>
namespace tacz::model {
inline bool cursorIconControl(std::string_view name) {
    return name=="selected_item_icon" || name=="inventory_selected_icon" || name=="inventory_selected_icon_button"
        || name.ends_with(".inventory_selected_icon") || name.ends_with(".inventory_selected_icon_button");
}
// The native flying renderer supplies already-eased slot coordinates/scale.
// Reject unusable values instead of suppressing origin with a near-zero draw.
inline std::optional<float> flyingIconSize(float x,float y,float scale) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(scale) || scale<0.1f || scale>16.0f) return std::nullopt;
    return 16.0f*scale;
}
}
