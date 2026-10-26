#include "pack/PackItems.h"
#include "pack/GunPackLoader.h"
#include "gun/ShootRules.h"
#include <algorithm>
#include <stdexcept>
namespace tacz::pack {
bool SlotIdentitySelection::offer(int renderId,int renderAux,int sourceId,int sourceAux,std::optional<ItemIdentity> const& source) {
    if (ambiguous || !source || renderId!=sourceId || renderAux!=sourceAux) return false;
    if (identity && identity!=source) { identity.reset(); ambiguous=true; return false; }
    identity=source; return true;
}
std::string_view itemType(ItemKind k) {
    switch (k) { case ItemKind::Gun:return "tacz:modern_kinetic_gun"; case ItemKind::Ammo:return "tacz:ammo"; default:return "tacz:attachment"; }
}
std::string_view identityKey(ItemKind k) {
    switch (k) { case ItemKind::Gun:return "GunId"; case ItemKind::Ammo:return "AmmoId"; default:return "AttachmentId"; }
}
std::optional<ItemIdentity> itemIdentity(nlohmann::json const& tag) {
    for (auto kind:{ItemKind::Gun,ItemKind::Ammo,ItemKind::Attachment}) {
        auto it=tag.find(identityKey(kind));
        if (it!=tag.end() && it->is_string()) if (auto id=ResourceId::parse(it->get<std::string>())) return ItemIdentity{kind,*id};
    }
    return std::nullopt;
}
nlohmann::json itemUserData(ItemIdentity const& identity,GunData const* gun) {
    nlohmann::json tag={{std::string(identityKey(identity.kind)),identity.id.toString()}};
    if (identity.kind==ItemKind::Gun) {
        if (!gun) throw std::invalid_argument("gun item needs creative-tab data");
        auto ammo=gun::initialAmmo(*gun);
        auto mode=gun->fireModes.empty() ? std::nullopt : gun->fireModes.front();
        tag["GunFireMode"]=mode==FireMode::Auto ? "AUTO" : mode==FireMode::Semi ? "SEMI" : mode==FireMode::Burst ? "BURST" : "UNKNOWN";
        tag["GunCurrentAmmoCount"]=ammo.magazine;
        tag["HasBulletInBarrel"]=ammo.barrel; // native bridge writes ByteTag
    }
    return tag;
}
int ammoStackLimit(int declared) { return declared<=0 ? 64 : std::clamp(declared,1,64); }
std::string stripFormatting(std::string_view text) {
    std::string out;
    for (size_t i=0;i<text.size();) {
        if (i+2<text.size() && text.substr(i,2)=="§") {
            auto code=text[i+2]; if (code>='A' && code<='Z') code+='a'-'A';
            if (std::string_view("0123456789abcdefghijklmnopqrstu").find(code)!=std::string_view::npos) { i+=3; continue; }
        }
        out+=text[i++];
    }
    return out;
}
nlohmann::json canonicalAmmoTag(nlohmann::json tag) {
    auto marker=tag.find("TaczAutoName");
    if (marker!=tag.end() && marker->is_string()) {
        auto display=tag.find("display");
        if (display!=tag.end() && display->is_object() && display->value("Name",nlohmann::json{})==*marker) {
            display->erase("Name");
            if (display->empty()) tag.erase("display");
        }
        tag.erase("TaczAutoName");
    }
    return tag;
}
std::vector<int> splitAmmo(int count,int limit) {
    std::vector<int> out;
    limit=ammoStackLimit(limit);
    while (count>0) { auto n=std::min(count,limit); out.push_back(n); count-=n; }
    return out;
}
int mergeAmmoCount(int target,int source,int limit,bool sameItem,bool sameTag) {
    return sameItem && sameTag ? std::min(std::max(0,source),std::max(0,ammoStackLimit(limit)-std::max(0,target))) : 0;
}
AmmoInsertion planAmmoInsertion(std::vector<AmmoSlot> const& slots,int count,int limit) {
    AmmoInsertion plan{std::vector<int>(slots.size(),0),std::max(0,count)};
    for (int phase=0;phase<2 && plan.remaining>0;++phase) for (size_t i=0;i<slots.size() && plan.remaining>0;++i) {
        bool empty=slots[i].count==0;
        if (empty!=(phase==1)) continue;
        int amount=mergeAmmoCount(slots[i].count,plan.remaining,limit,empty || slots[i].compatible,true);
        plan.additions[i]=amount; plan.remaining-=amount;
    }
    return plan;
}
PickupPlan planPickup(int groundCount,int capacity) {
    groundCount=std::max(0,groundCount);
    int accepted=std::min(groundCount,std::max(0,capacity));
    return {accepted,groundCount-accepted};
}
bool nativeAmmoAddComplete(int remaining) { return remaining==0; }
bool preserveAmmoPickupRemainder(bool samePlayer,int inserted,int remaining) {
    return samePlayer && inserted>=0 && remaining>0;
}
int ammoPickupGroundAfter(int rawAfterTake,bool removed,bool nativeComplete) {
    return removed || nativeComplete ? 0 : std::max(0,rawAfterTake);
}
std::string normalizeLanguage(std::string code) {
    for (auto& c:code) { if (c=='-') c='_'; else if (c>='A' && c<='Z') c+= 'a'-'A'; }
    return code;
}
std::string lookupTranslation(LanguageTable const& languages,std::string const& key,std::string const& locale) {
    for (auto const& code:{normalizeLanguage(locale),std::string("en_us")})
        if (auto l=languages.find(code);l!=languages.end()) if (auto v=l->second.find(key);v!=l->second.end()) return v->second;
    return key;
}
std::string itemName(GunPackRegistry const& packs,ItemIdentity const& identity,std::string const& locale) {
    std::string key=identity.id.toString();
    if (identity.kind==ItemKind::Gun) { if (auto g=packs.gun(identity.id); g && !g->index.name.empty()) key=g->index.name; }
    else if (identity.kind==ItemKind::Ammo) { if (auto a=packs.ammo(identity.id); a && !a->index.name.empty()) key=a->index.name; }
    return stripFormatting(packs.localize(key,locale));
}
std::optional<std::string> itemIcon(GunPackRegistry const& packs,ItemIdentity const& identity) {
    std::optional<ResourceId> slot;
    if (identity.kind==ItemKind::Gun) {
        auto gun=packs.gun(identity.id);
        auto display=gun && gun->index.display ? packs.gunDisplay(*gun->index.display) : nullptr;
        if (display) if (auto it=display->raw.find("slot");it!=display->raw.end() && it->is_string()) slot=ResourceId::parse(it->get<std::string>());
    } else if (identity.kind==ItemKind::Ammo) slot=packs.ammoSlot(identity.id);
    if (!slot) return std::nullopt;
    auto path=assetPath(*slot,"textures",".png");
    return packs.hasFile(path) ? std::optional(path) : std::nullopt;
}
}
