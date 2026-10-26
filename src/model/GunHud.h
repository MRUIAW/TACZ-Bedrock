#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include "gun/ShootRules.h"

namespace tacz::model::hud {
inline constexpr int MaxAmmo=9999;
inline int currentAmmo(int magazine,bool barrel,std::optional<pack::Bolt> bolt) {
    return static_cast<int>(std::clamp<int64_t>(int64_t(magazine)+(gun::hasChamberedRound(bolt,barrel) ? 1 : 0),0,MaxAmmo));
}
// GunHudOverlay: both strict comparisons, and the chamber round is included.
inline bool lowAmmo(int ammo,int capacity) { return ammo<10 && int64_t(ammo)*4<capacity; }
inline std::string padded(int value,size_t width) {
    auto text=std::to_string(std::clamp(value,0,MaxAmmo));
    return text.size()<width ? std::string(width-text.size(),'0')+text : text;
}
inline std::string ammoText(int ammo,int capacity,bool percent) {
    if (!percent) return padded(ammo,3);
    // Java first divides as float, then DecimalFormat("000%") multiplies as
    // double and rounds ties even. Preserve float error (1/40 -> 003%, not 002%).
    float ratio=static_cast<float>(std::clamp(ammo,0,MaxAmmo))/static_cast<float>(std::max(capacity,1));
    double scaled=static_cast<double>(ratio)*100;
    auto value=static_cast<int64_t>(std::floor(scaled));
    double remainder=scaled-static_cast<double>(value);
    if (remainder>.5 || (remainder==.5 && value%2)) ++value;
    auto text=std::to_string(value);
    return (text.size()<3 ? std::string(3-text.size(),'0') : "")+text+"%";
}
inline std::string reserveText(int reserve,bool creative,bool infinite,bool inventoryFed) {
    if (inventoryFed) return {};
    if (infinite) return "\xE2\x88\x9E";
    return padded(creative ? MaxAmmo : reserve,4);
}
struct CrosshairState {
    bool holding=false,firstPerson=true,hideHud=false,spectator=false;
    bool reloading=false,animationHide=false,puttingAway=false,forceInAds=false;
    float aim=0;
};
inline bool crosshairVisible(CrosshairState const& s) {
    return s.holding && s.firstPerson && !s.hideHud && !s.spectator && !s.reloading
        && !s.animationHide && !s.puttingAway && (s.aim<=.9f || s.forceInAds);
}
inline constexpr std::array<std::string_view,22> Crosshairs={"empty","dot_1","circle_1","circle_2","circle_3",
    "cross_1","cross_2","cross_3","cross_4","cross_5","cross_6","line_1","line_2","line_3",
    "square_1","square_2","square_3","square_4","square_5","square_6","trident_1","trident_2"};
inline std::string crosshairType(std::string value) {
    for (char& c:value) if (c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
    return std::find(Crosshairs.begin(),Crosshairs.end(),value)!=Crosshairs.end() ? value : "dot_1";
}
struct HitMarker {
    std::optional<int64_t> hit,kill,head;
    void mark(int64_t now,bool killed,bool headshot) {
        (killed ? kill : hit)=now;
        if (headshot) head=now;
    }
    struct Frame { float alpha=0,offset=4; bool red=false; };
    Frame sample(int64_t now) const {
        auto age=[&](auto const& at) { return at && now>=*at ? now-*at : int64_t(301); };
        bool killing=age(kill)<=300;
        auto elapsed=killing ? age(kill) : age(hit);
        if (elapsed>300) return {};
        return {1.f-static_cast<float>(elapsed)/300.f,4.f+(killing ? static_cast<float>(elapsed)*4.f/300.f : 0.f),age(head)<=300};
    }
};
} // namespace tacz::model::hud
