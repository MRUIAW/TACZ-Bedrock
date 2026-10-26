#include "pack/PackItems.h"
#include "pack/GunPackLoader.h"
#include "gun/ShootRules.h"
#include <algorithm>
#include <stdexcept>
namespace tacz::pack {
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
    return packs.localize(key,locale);
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
