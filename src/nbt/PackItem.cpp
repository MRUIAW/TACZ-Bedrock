#include "nbt/PackItem.h"
#include "nbt/AmmoItem.h"
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
#include "mc/safety/RedactableString.h"
#include "pack/ItemTooltip.h"
#include "client/ui/GunHud.h"
#include <atomic>
#include <cmath>
#include <limits>
#endif
#include "ll/api/service/Bedrock.h"
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
    // No language-fixed display.Name: clients resolve the pack name at hover time.
    stack.setUserData(std::move(tag));
    return player.addAndRefresh(stack);
}
namespace {
static_assert(std::is_same_v<decltype(&ItemStackBase::getMaxStackSize),uchar (ItemStackBase::*)() const>);
#ifdef TACZ_CLIENT
static_assert(std::is_same_v<decltype(&ItemStackBase::getHoverName),std::string (ItemStackBase::*)() const>);
#endif
LL_TYPE_INSTANCE_HOOK(PackStackLimit,ll::memory::HookPriority::Normal,ItemStackBase,&ItemStackBase::getMaxStackSize,uchar) {
#ifdef TACZ_CLIENT
    if (!client::shooter::worldToken() && !ll::service::getLevel()) return origin();
#else
    if (!ll::service::getLevel()) return origin();
#endif
    auto identity=readItemIdentity(*this);
    if (identity && identity->kind==pack::ItemKind::Ammo) if (auto ammo=TaczMod::getInstance().gunPacks().ammo(identity->id))
        return static_cast<uchar>(pack::ammoStackLimit(ammo->index.stackSize));
    return origin();
}
#ifdef TACZ_CLIENT
std::string currentLanguage() {
    return getI18n().getCurrentLanguage()->getFullLanguageCode(); // SDK returns not_null
}
std::string realCustomName(ItemStackBase const& stack) {
    auto name=stack.getCustomName();
    if (!name.empty() && stack.mUserData && stack.mUserData->contains("TaczAutoName",Tag::String)
        && name==static_cast<std::string const&>((*stack.mUserData)["TaczAutoName"])) return {};
    return name;
}
std::optional<std::string> resolvedName(ItemStackBase const& stack) {
    if (!client::shooter::worldToken()) return std::nullopt;
    auto identity=readItemIdentity(stack); if (!identity) return std::nullopt;
    auto custom=realCustomName(stack); auto locale=currentLanguage();
    auto name=custom.empty() ? pack::itemName(TaczMod::getInstance().gunPacks(),*identity,locale) : custom;
    static std::atomic<int> samples=0;
    if (samples.fetch_add(1)<12) TaczMod::getInstance().getSelf().getLogger().info(
        "[pack-items] name id={} language={} text={}",identity->id.toString(),locale,name);
    return name;
}
LL_TYPE_INSTANCE_HOOK(PackHoverName,ll::memory::HookPriority::Normal,ItemStackBase,&ItemStackBase::getHoverName,std::string) {
    auto name=resolvedName(*this); return name ? "§r§f"+pack::stripFormatting(*name) : origin();
}
using Redacted=Bedrock::Safety::RedactableString;
static_assert(std::is_same_v<decltype(&ItemStackBase::getName),std::string (ItemStackBase::*)() const>);
static_assert(std::is_same_v<decltype(&ItemStackBase::getRedactedHoverName),Redacted (ItemStackBase::*)() const>);
static_assert(std::is_same_v<decltype(&ItemStackBase::getRedactedName),Redacted (ItemStackBase::*)() const>);
static_assert(std::is_same_v<decltype(&ItemStackBase::getFormattedHovertext),Redacted (ItemStackBase::*)(Level&,bool) const>);
static_assert(std::is_same_v<decltype(&Localization::getFullLanguageCode),std::string (Localization::*)() const>);
LL_TYPE_INSTANCE_HOOK(PackName,ll::memory::HookPriority::Normal,ItemStackBase,&ItemStackBase::getName,std::string) {
    auto name=resolvedName(*this); return name ? "§r§f"+pack::stripFormatting(*name) : origin();
}
LL_TYPE_INSTANCE_HOOK(PackRedactedHover,ll::memory::HookPriority::Normal,ItemStackBase,&ItemStackBase::getRedactedHoverName,Redacted) {
    auto name=resolvedName(*this); return name ? Redacted("§r§f"+pack::stripFormatting(*name)) : origin();
}
LL_TYPE_INSTANCE_HOOK(PackRedactedName,ll::memory::HookPriority::Normal,ItemStackBase,&ItemStackBase::getRedactedName,Redacted) {
    auto name=resolvedName(*this); return name ? Redacted("§r§f"+pack::stripFormatting(*name)) : origin();
}
LL_TYPE_INSTANCE_HOOK(PackTooltip,ll::memory::HookPriority::Normal,ItemStackBase,&ItemStackBase::getFormattedHovertext,Redacted,Level& level,bool category) {
    if (!client::shooter::worldToken()) return origin(level,category);
    auto identity=readItemIdentity(*this); if (!identity) return origin(level,category);
    try {
        auto locale=currentLanguage();
        pack::TooltipState state;
        if (mUserData) {
            auto number=[&](std::string_view key,int fallback=0) {
                if (!mUserData->contains(key)) return fallback;
                auto const& value=(*mUserData)[key]; if (!value.is_number()) return fallback;
                if (mUserData->contains(key,Tag::Int64)) return static_cast<int>(static_cast<int64_t>(value));
                double n=static_cast<double>(value);
                if (std::isnan(n)) return 0;
                return static_cast<int>(std::clamp(std::floor(n),double(std::numeric_limits<int>::min()),double(std::numeric_limits<int>::max())));
            };
            state.ammo=number("GunCurrentAmmoCount"); state.chamber=number("HasBulletInBarrel")!=0;
            state.hideFlags=static_cast<unsigned>(number("HideFlags")); // Java item default=0, any numeric tag
            if (mUserData->contains("DummyAmmo",Tag::Int)) state.dummy=number("DummyAmmo");
            if (mUserData->contains("GunFireMode",Tag::String)) {
                std::string mode=static_cast<std::string const&>((*mUserData)["GunFireMode"]);
                if (mode=="AUTO") state.mode=pack::FireMode::Auto;
                if (mode=="SEMI") state.mode=pack::FireMode::Semi;
                if (mode=="BURST") state.mode=pack::FireMode::Burst;
            }
        }
        auto tooltip=pack::buildItemTooltip(TaczMod::getInstance().gunPacks(),*identity,locale,state,realCustomName(*this));
        for (auto const& lore:getCustomLore()) tooltip.lines.push_back("§r§7"+lore+"§r");
        auto text=tooltip.text();
        if (tooltip.ammoIcon) client::hud::rememberTooltip(text,*tooltip.ammoIcon);
        static std::atomic<int> samples=0;
        if (samples.fetch_add(1)<12) TaczMod::getInstance().getSelf().getLogger().info(
            "[pack-items] hover id={} language={} lines={} ammo={} chamber={} hideFlags={}",identity->id.toString(),locale,tooltip.lines.size(),state.ammo,state.chamber,state.hideFlags);
        return Redacted(text);
    } catch (std::exception const& e) {
        static std::atomic<bool> warned=false;
        if (!warned.exchange(true)) TaczMod::getInstance().getSelf().getLogger().warn("[pack-items] tooltip fallback: {}",e.what());
        return origin(level,category);
    }
}
#endif
bool stackHook=false,nameHook=false,plainNameHook=false,redactedHoverHook=false,redactedNameHook=false,tooltipHook=false;
}
void enablePackItems() {
    enableAmmoInventory();
    stackHook=PackStackLimit::hook()==0;
#ifdef TACZ_CLIENT
    nameHook=PackHoverName::hook()==0;
    plainNameHook=PackName::hook()==0;
    redactedHoverHook=PackRedactedHover::hook()==0;
    redactedNameHook=PackRedactedName::hook()==0;
    tooltipHook=PackTooltip::hook()==0;
#endif
    auto& mod=TaczMod::getInstance();
    mod.getSelf().getLogger().info("[pack-items] stackLimit={} hoverName={} name={} redactedHover={} redactedName={} tooltip={}",stackHook,nameHook,plainNameHook,redactedHoverHook,redactedNameHook,tooltipHook);
    for (auto const& [id,ammo]:mod.gunPacks().ammos()) if (ammo.index.stackSize>64)
        mod.getSelf().getLogger().warn("[pack-items] {} stack_size={} exceeds Bedrock1..64; using64",id.toString(),ammo.index.stackSize);
}
void disablePackItems() {
    disableAmmoInventory();
    if (stackHook) PackStackLimit::unhook();
#ifdef TACZ_CLIENT
    if (nameHook) PackHoverName::unhook();
    if (plainNameHook) PackName::unhook();
    if (redactedHoverHook) PackRedactedHover::unhook();
    if (redactedNameHook) PackRedactedName::unhook();
    if (tooltipHook) PackTooltip::unhook();
#endif
    stackHook=nameHook=plainNameHook=redactedHoverHook=redactedNameHook=tooltipHook=false;
}
}
