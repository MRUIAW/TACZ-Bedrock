#pragma once
#include <cmath>
#include <string>
#include <string_view>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include "model/GunHud.h"

namespace tacz::model::hud {
// Java GUI units are Bedrock UI units here, NOT physical screen pixels. Native
// UI applies mGuiScale at submission; scaling these positions again doubles it.
struct Layout {
    glm::vec4 line; // left, top, right, bottom
    glm::vec2 current,reserve,version,gunIcon,fireMode;
    static constexpr float CurrentScale=1.5f,ReserveScale=.8f,VersionScale=.5f;
    static constexpr glm::vec2 GunSize{39,13},ModeSize{10,10};
};
inline Layout layout(float w,float h,float unscaledCurrentWidth) {
    return {{w-75,h-43,w-74,h-25},{w-70,h-43},{w-68+unscaledCurrentWidth*1.5f,h-43},
        {std::trunc((w-70)/.5f)*.5f,std::trunc((h-29)/.5f)*.5f},{w-117,h-44},
        {std::trunc(w-68.5f+unscaledCurrentWidth*1.5f),h-38}};
}
// Minecraft bitmap digits have six GUI units of advance. Normalize the game's
// original bitmap font metrics rather than using a smooth/default UI override.
inline float bitmapScale(float digitAdvance) {
    return std::isfinite(digitAdvance) && digitAdvance>0 ? 6.f/digitAdvance : 1.f;
}
inline uint32_t currentColor(int ammo,int capacity,bool inventory,bool dummy,bool overheat=false) {
    if (lowAmmo(ammo,capacity) || overheat) return 0xFF5555;
    return inventory ? (dummy ? 0x55FFFF : 0xFFFF55) : 0xFFFFFF;
}
inline uint32_t reserveColor(bool inventory,bool dummy) { return !inventory && dummy ? 0x55FFFF : 0xAAAAAA; }
inline std::string versionText(std::string_view game,std::string_view mod) {
    // Display release triplet, without a launcher/revision suffix: 1.26.51-0.3.21.
    if (game.starts_with('v')) game.remove_prefix(1);
    size_t end=0; int dots=0;
    for (;end<game.size();++end) {
        char c=game[end];
        if (c=='.' && dots<2) ++dots;
        else if (c<'0' || c>'9') break;
    }
    return std::string(game.substr(0,end))+"-"+std::string(mod);
}
} // namespace tacz::model::hud
