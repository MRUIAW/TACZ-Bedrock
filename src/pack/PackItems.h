#pragma once
#include "pack/GunData.h"
namespace tacz::pack {
class GunPackRegistry;
enum class ItemKind { Gun,Ammo,Attachment };
struct ItemIdentity { ItemKind kind; ResourceId id; bool operator==(ItemIdentity const&) const=default; };
std::string_view itemType(ItemKind kind);
std::string_view identityKey(ItemKind kind);
std::optional<ItemIdentity> itemIdentity(nlohmann::json const& tag);
nlohmann::json itemUserData(ItemIdentity const& identity,GunData const* gun=nullptr);
int ammoStackLimit(int declared); // Bedrock data-driven items support1..64
std::string normalizeLanguage(std::string code);
using LanguageTable=std::map<std::string,std::map<std::string,std::string>>;
std::string lookupTranslation(LanguageTable const& languages,std::string const& key,std::string const& locale);
std::string itemName(GunPackRegistry const& packs,ItemIdentity const&,std::string const& locale);
std::optional<std::string> itemIcon(GunPackRegistry const& packs,ItemIdentity const&);
}
