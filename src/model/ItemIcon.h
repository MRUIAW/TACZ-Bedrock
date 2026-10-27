#pragma once
#include <string_view>
namespace tacz::model {
inline bool cursorIconControl(std::string_view name) {
    return name=="selected_item_icon" || name=="inventory_selected_icon" || name=="inventory_selected_icon_button"
        || name.ends_with(".inventory_selected_icon") || name.ends_with(".inventory_selected_icon_button");
}
}
