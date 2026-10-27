#pragma once
#include <cstdint>
#include <span>
#include <string_view>
namespace tacz::model {
inline constexpr std::string_view SlotIconMaterial="ui_textured_and_glcolor_sprite";
struct IconAlpha { size_t transparent=0,partial=0,opaque=0; bool valid=false; };
inline IconAlpha iconAlpha(std::span<uint8_t const> rgba) {
    IconAlpha out; if (rgba.empty() || rgba.size()%4) return out;
    out.valid=true;
    for (size_t i=3;i<rgba.size();i+=4) {
        if (!rgba[i]) ++out.transparent;
        else if (rgba[i]==255) ++out.opaque;
        else ++out.partial;
    }
    return out;
}
inline bool cursorIconControl(std::string_view name) {
    return name=="selected_item_icon" || name=="inventory_selected_icon" || name=="inventory_selected_icon_button"
        || name.ends_with(".inventory_selected_icon") || name.ends_with(".inventory_selected_icon_button");
}
}
