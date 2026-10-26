#include "pack/BuiltinAssets.h"

#include "TaczBuiltinAssets.h"

namespace tacz::pack {
std::optional<std::string_view> builtinAsset(std::string_view path) {
    if (path == "assets/tacz/animations/rifle_default.animation.json") return builtin::rifle_default;
    if (path == "assets/tacz/animations/pistol_default.animation.json") return builtin::pistol_default;
    if (path == "assets/tacz/scripts/default_state_machine.lua") return builtin::default_state_machine;
    for (auto const& image:builtin::images) if (path==image.path)
        return std::string_view(reinterpret_cast<char const*>(image.data),image.size);
    return std::nullopt;
}
}
