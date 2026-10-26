#pragma once
#include "pack/GunPackLoader.h"
namespace tacz::pack {
inline constexpr std::string_view AmmoIconMarker="§r§0§r"; // formatting only; reserves no visible glyph
struct TooltipState {
    int ammo=0;
    bool chamber=false;
    std::optional<FireMode> mode;
    unsigned hideFlags=0;
    std::optional<int> dummy;
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
