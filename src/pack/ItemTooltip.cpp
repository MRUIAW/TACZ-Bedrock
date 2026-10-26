#include "pack/ItemTooltip.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
namespace tacz::pack {
namespace {
std::string decimal(double n,int digits) {
    if (!std::isfinite(n)) n=0;
    std::ostringstream out; out.imbue(std::locale::classic());
    out<<std::fixed<<std::setprecision(digits)<<n;
    auto s=out.str();
    if (s.find('.')!=std::string::npos) { while (s.ends_with('0')) s.pop_back(); if (s.ends_with('.')) s.pop_back(); }
    if (s=="-0") s="0";
    return s;
}
std::string percent(double n,int digits=2) { return decimal(n*100,digits)+"%"; }
std::string argument(std::string s,std::string const& arg) {
    auto at=s.find("%s"); if (at!=std::string::npos) s.replace(at,2,arg); return s;
}
}
std::string ItemTooltip::text() const {
    std::string out;
    for (auto const& line:lines) { if (!out.empty()) out+='\n'; out+=line; }
    return out;
}
std::optional<int> tooltipAmmoIconLine(std::string_view drawn,std::string_view built) {
    auto marker=built.find(AmmoIconMarker);
    if (marker==std::string_view::npos) return std::nullopt;
    auto at=drawn.find(built.substr(marker));
    if (at==std::string_view::npos) return std::nullopt;
    return static_cast<int>(std::count(drawn.begin(),drawn.begin()+at,'\n'));
}
ItemTooltip buildItemTooltip(GunPackRegistry const& packs,ItemIdentity const& identity,std::string const& locale,
    TooltipState const& state,std::string const& customName) {
    ItemTooltip out;
    auto tr=[&](std::string const& key) { return packs.localize(key,locale); };
    auto line=[&](std::string s) { out.lines.push_back(std::move(s)+"§r"); };
    auto title=customName.empty() ? itemName(packs,identity,locale) : stripFormatting(customName);
    line("§r§f"+title); // common rarity; pack-embedded colour cannot leak into title
    auto packName=packs.packDisplayName(identity.id,locale);
    if (identity.kind==ItemKind::Ammo) {
        if (auto ammo=packs.ammo(identity.id); ammo && !ammo->index.tooltip.empty()) line("§7"+tr(ammo->index.tooltip));
        line("§9"+tr("itemGroup.tab.tacz.ammo"));
        if (!packName.empty()) line("§9§o"+packName);
        return out;
    }
    auto gun=identity.kind==ItemKind::Gun ? packs.gun(identity.id) : nullptr;
    if (!gun) return out;
    auto const& data=gun->data;
    auto display=gun->index.display ? packs.gunDisplay(*gun->index.display) : nullptr;
    auto shown=[&](unsigned bit) { return !(state.hideFlags&(1u<<bit)); };
    auto type=tr("tacz.type."+gun->index.type+".name");
    if (shown(0) && !gun->index.tooltip.empty()) line("§7"+tr(gun->index.tooltip));
    if (shown(1) && data.ammoId) {
        ItemIdentity ammo{ItemKind::Ammo,*data.ammoId};
        out.ammoIcon=itemIcon(packs,ammo);
        auto indent=out.ammoIcon ? std::string(AmmoIconMarker)+"     " : std::string{};
        // The title is forced white, but inside the Java gun panel the pack's
        // ammo-name formatting is retained on top of its orange base colour.
        auto entry=packs.ammo(*data.ammoId);
        line(indent+"§6"+(entry ? tr(entry->index.name) : itemName(packs,ammo,locale)));
        int chamber=state.chamber && data.bolt!=Bolt::OpenBolt ? 1 : 0;
        int current=std::max(0,state.ammo)+chamber,max=data.ammoAmount+chamber;
        auto count=display && display->ammoPercent ? percent(double(current)/std::max(1,max),0)
            : std::to_string(current)+"/"+std::to_string(max);
        if (state.dummy) count+=" ("+std::to_string(std::max(0,*state.dummy))+")";
        line(std::string(out.ammoIcon ? "     " : "")+(data.reload.type==FeedType::Inventory ? "§e"+tr("tooltip.tacz.gun.inventory_mode") : "§8"+count));
    }
    auto adjust=data.fireModeAdjustFor(state.mode);
    auto const& extra=data.bullet.extraDamage;
    double damage=extra && !extra->damageAdjust.empty() ? extra->damageAdjust.front().damage : data.bullet.damage;
    if (adjust) damage+=adjust->damage;
    if (shown(2)) {
        line("§8"+tr("tooltip.tacz.gun.level")+"§5"+"0 (MAX)"); // ModernKineticGunItem: level=maxLevel=0
        line("§8"+tr("tooltip.tacz.gun.type")+"§b"+type);
        auto damageText=decimal(damage,2);
        auto style=display ? display->raw.find("damage_style") : nlohmann::json::const_iterator{};
        if (display && style!=display->raw.end() && style->is_string() && *style=="per_projectile" && data.bullet.bulletAmount>1)
            damageText=decimal(damage/data.bullet.bulletAmount,2)+"x"+std::to_string(data.bullet.bulletAmount);
        if (data.bullet.explosion && data.bullet.explosion->explode)
            damageText+=" + "+decimal(data.bullet.explosion->damage,2)+tr("tooltip.tacz.gun.explosion");
        line("§8"+tr("tooltip.tacz.gun.damage")+"§b"+damageText);
    }
    if (shown(3)) {
        double armor=extra ? std::clamp(double(extra->armorIgnore)+(adjust ? adjust->armorIgnore : 0),0.,1.) : 0;
        double head=extra ? double(extra->headShotMultiplier)+(adjust ? adjust->headShotMultiplier : 0) : 1;
        line("§6"+argument(tr("tooltip.tacz.gun.armor_ignore"),percent(armor)));
        line("§6"+argument(tr("tooltip.tacz.gun.head_shot_multiplier"),percent(head)));
        line("§c"+argument(tr("tooltip.tacz.gun.movement_speed"),percent(-.015*data.weight,1)));
    }
    if (shown(4)) line("§e§o"+argument(tr("tooltip.tacz.gun.tips"),"Z")); // display compatibility only; refit is not implemented
    if (shown(5) && !packName.empty()) line("§9§o"+packName);
    line("§9"+type); // creative/type footer as in the Java inventory tooltip
    return out;
}
}
