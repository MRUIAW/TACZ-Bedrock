#pragma once
#include "pack/GunPackLoader.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
namespace tacz::pack {
// SDK IntTag/ByteTag only convert to integral types. Variant -> double silently
// returns 0 for them; read the concrete tag's scalar field instead.
template<class NumericTag> int tooltipTagInteger(NumericTag const& tag) {
    if constexpr (std::is_integral_v<std::remove_cvref_t<decltype(tag.data)>>) return static_cast<int>(tag.data);
    else {
        double n=tag.data;
        if (std::isnan(n)) return 0;
        return static_cast<int>(std::clamp(std::floor(n),double(std::numeric_limits<int>::min()),double(std::numeric_limits<int>::max())));
    }
}
inline constexpr std::string_view AmmoIconMarker="§r§0§r"; // formatting only; reserves no visible glyph
struct TooltipState {
    int ammo=0;
    bool chamber=false;
    std::optional<FireMode> mode;
    unsigned hideFlags=0;
    std::optional<int> dummy;
    std::optional<int> capacity; // effective attachment capacity, excluding chamber
    // Attachment mechanics are not implemented yet. Values describe the
    // effective, currently supported gun, not imaginary installed upgrades.
};
struct ItemTooltip {
    std::vector<std::string> lines;
    std::optional<std::string> ammoIcon;
    std::string text() const;
};
ItemTooltip buildItemTooltip(GunPackRegistry const&,ItemIdentity const&,std::string const& locale,
    TooltipState const& state={},std::string const& customName={});
std::optional<int> tooltipAmmoIconLine(std::string_view drawn,std::string_view built);
}
