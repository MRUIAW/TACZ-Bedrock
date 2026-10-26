#include "nbt/PackItem.h"
#include "ll/api/memory/Hook.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/actor/player/Player.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/ByteTag.h"
#include "mod/TaczMod.h"
#ifdef TACZ_CLIENT
#include "client/gameplay/ClientShooter.h"
#include "mc/locale/I18n.h"
#include "mc/locale/Localization.h"
#endif
#include <type_traits>
namespace tacz::nbt {
std::optional<pack::ItemIdentity> readItemIdentity(ItemStackBase const& item) {
    if (item.isNull() || !item.mUserData) return std::nullopt;
    for (auto kind:{pack::ItemKind::Gun,pack::ItemKind::Ammo,pack::ItemKind::Attachment}) {
        auto key=pack::identityKey(kind);
        if (item.mUserData->contains(key,Tag::String))
            if (auto id=pack::ResourceId::parse(static_cast<std::string const&>((*item.mUserData)[key]))) return pack::ItemIdentity{kind,*id};
    }
    return std::nullopt; // legacy vanilla stacks with TACZ identity stay readable
}
std::optional<pack::ResourceId> readAttachmentId(ItemStackBase const& item) {
    auto identity=readItemIdentity(item);
    return identity && identity->kind==pack::ItemKind::Attachment ? std::optional(identity->id) : std::nullopt;
}
void writeAttachmentId(ItemStack& item,pack::ResourceId const& id) {
    auto tag=item.mUserData ? item.mUserData->clone() : std::make_unique<CompoundTag>();
    (*tag)["AttachmentId"]=StringTag{id.toString()}; item.setUserData(std::move(tag));
}
bool giveGun(Player& player,pack::ResourceId const& id) {
    auto const& packs=TaczMod::getInstance().gunPacks();
    auto gun=packs.gun(id); if (!gun) return false;
    ItemStack stack;
    stack.reinit(std::string(pack::itemType(pack::ItemKind::Gun)),1,0);
    if (stack.isNull()) return false; // missing BP: never silently substitute vanilla
    auto values=pack::itemUserData({pack::ItemKind::Gun,id},&gun->data);
    auto tag=std::make_unique<CompoundTag>();
    for (auto it=values.begin();it!=values.end();++it) {
        if (it->is_boolean()) (*tag)[it.key()]=ByteTag{it->get<bool>() ? 1 : 0};
        else if (it->is_number_integer()) (*tag)[it.key()]=IntTag{it->get<int>()};
        else (*tag)[it.key()]=StringTag{it->get<std::string>()};
    }
    auto name=pack::itemName(packs,{pack::ItemKind::Gun,id},"en_us");
    (*tag)["display"]=CompoundTag{{"Name",StringTag{name}}};
    (*tag)["TaczAutoName"]=StringTag{name};
    stack.setUserData(std::move(tag));
    return player.addAndRefresh(stack);
}
namespace {
static_assert(std::is_same_v<decltype(&ItemStackBase::getMaxStackSize),uchar (ItemStackBase::*)() const>);
#ifdef TACZ_CLIENT
static_assert(std::is_same_v<decltype(&ItemStackBase::getHoverName),std::string (ItemStackBase::*)() const>);
#endif
LL_TYPE_INSTANCE_HOOK(PackStackLimit,ll::memory::HookPriority::Normal,ItemStackBase,&ItemStackBase::getMaxStackSize,uchar) {
    auto identity=readItemIdentity(*this);
    if (identity && identity->kind==pack::ItemKind::Ammo) if (auto ammo=TaczMod::getInstance().gunPacks().ammo(identity->id))
        return static_cast<uchar>(pack::ammoStackLimit(ammo->index.stackSize));
    return origin();
}
#ifdef TACZ_CLIENT
LL_TYPE_INSTANCE_HOOK(PackHoverName,ll::memory::HookPriority::Normal,ItemStackBase,&ItemStackBase::getHoverName,std::string) {
    if (!client::shooter::worldToken()) return origin();
    auto identity=readItemIdentity(*this);
    if (!identity) return origin();
    // Preserve real anvil/custom names, but allow our issued en_us fallback name
    // to track the client's current locale without rewriting inventory NBT.
    auto custom=getCustomName();
    if (!custom.empty() && (!mUserData->contains("TaczAutoName",Tag::String)
        || custom!=static_cast<std::string const&>((*mUserData)["TaczAutoName"]))) return origin();
    std::string locale="en_us";
    locale=getI18n().getCurrentLanguage()->mCode;
    return pack::itemName(TaczMod::getInstance().gunPacks(),*identity,locale);
}
#endif
bool stackHook=false,nameHook=false;
}
void enablePackItems() {
    stackHook=PackStackLimit::hook()==0;
#ifdef TACZ_CLIENT
    nameHook=PackHoverName::hook()==0;
#endif
    auto& mod=TaczMod::getInstance();
    mod.getSelf().getLogger().info("[pack-items] stackLimitHook={} hoverNameHook={}",stackHook,nameHook);
    for (auto const& [id,ammo]:mod.gunPacks().ammos()) if (ammo.index.stackSize>64)
        mod.getSelf().getLogger().warn("[pack-items] {} stack_size={} exceeds Bedrock1..64; using64",id.toString(),ammo.index.stackSize);
}
void disablePackItems() {
    if (stackHook) PackStackLimit::unhook();
#ifdef TACZ_CLIENT
    if (nameHook) PackHoverName::unhook();
#endif
    stackHook=nameHook=false;
}
}
