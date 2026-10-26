#include "nbt/AmmoItem.h"

#include <algorithm>
#include <memory>
#include <string>
#include <atomic>
#include <type_traits>
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/Container.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include "nbt/GunNbt.h"
#include "pack/PackItems.h"
#include "mod/TaczMod.h"
#ifdef TACZ_CLIENT
#include "client/gameplay/ClientShooter.h"
#endif

namespace tacz::nbt {
namespace {
// Match the pure canonicalAmmoTag rule without serializing or dropping NBT types.
std::unique_ptr<CompoundTag> canonicalTag(ItemStackBase const& item) {
    auto tag=item.mUserData ? item.mUserData->clone() : std::make_unique<CompoundTag>();
    if (!tag->contains("TaczAutoName",Tag::String)) return tag;
    auto name=static_cast<std::string const&>((*tag)["TaczAutoName"]);
    if (tag->contains("display",Tag::Compound)) {
        auto& display=(*tag)["display"].get<CompoundTag>();
        if (display.contains("Name",Tag::String) && static_cast<std::string const&>(display["Name"])==name) {
            display.erase("Name");
            if (display.empty()) tag->erase("display");
        }
    }
    tag->erase("TaczAutoName"); return tag;
}
ItemStack canonicalAmmo(ItemStack const& item) {
    ItemStack copy(item); copy.setUserData(canonicalTag(item));
    return copy;
}
int limitFor(ItemStackBase const& item) {
#ifdef TACZ_CLIENT
    if (!client::shooter::worldToken() && !ll::service::getLevel()) return 0;
#else
    if (!ll::service::getLevel()) return 0;
#endif
    auto id=readAmmoId(item); if (!id) return 0;
    auto ammo=TaczMod::getInstance().gunPacks().ammo(*id);
    return ammo ? pack::ammoStackLimit(ammo->index.stackSize) : 0;
}
bool compatible(ItemStack const& a,ItemStack const& b) {
    return a.getTypeName()==b.getTypeName() && a.getAuxValue()==b.getAuxValue() && a.hasSameUserData(b);
}
// Both prediction and mutation use exactly the same capacity/merge rule. canAdd
// never edits inventory or the caller's NBT; partial pickup is allowed like vanilla.
pack::AmmoInsertion insertionPlan(Inventory const& inventory,ItemStack const& source,int limit) {
    auto incoming=canonicalAmmo(source);
    std::vector<pack::AmmoSlot> slots;
    for (int slot=0;slot<inventory.getContainerSize();++slot) {
        auto const& target=inventory.getItem(slot);
        if (target.isNull()) slots.push_back({0,false});
        else {
            auto clean=canonicalAmmo(target);
            slots.push_back({target.mCount,compatible(clean,incoming)});
        }
    }
    return pack::planAmmoInsertion(slots,source.mCount,limit);
}
int insertAmmo(Inventory& inventory,ItemStack& source,int limit) {
    auto incoming=canonicalAmmo(source);
    auto plan=insertionPlan(inventory,source,limit);
    // Merge all compatible stacks first, then use empty slots. Do not replace
    // unrelated NBT/custom names, and do not silently transform legacy base items.
    for (int slot=0;slot<static_cast<int>(plan.additions.size());++slot) {
        int amount=plan.additions[slot]; if (!amount) continue;
        auto const& target=inventory.getItem(slot);
        bool empty=target.isNull();
        auto copy=empty ? ItemStack(incoming) : canonicalAmmo(target);
        int old=empty ? 0 : target.mCount;
        copy.set(old+amount); inventory.setItem(slot,copy);
    }
    int inserted=int(source.mCount)-plan.remaining;
    source.set(plan.remaining); // leave uninserted rounds in the ground/caller stack
    return inserted;
}
std::atomic<int> inventorySamples=0;
static_assert(std::is_same_v<decltype(&ItemStackBase::hasSameUserData),bool (ItemStackBase::*)(ItemStackBase const&) const>);
LL_TYPE_INSTANCE_HOOK(AmmoSameData,ll::memory::HookPriority::Normal,ItemStackBase,&ItemStackBase::hasSameUserData,bool,ItemStackBase const& other) {
    if (!limitFor(*this) || !limitFor(other)) return origin(other);
    // Native drag/container paths can compare NBT without going through Inventory::add.
    // Do not override item/aux checks or any native read-only stackability predicate.
    auto a=canonicalTag(*this),b=canonicalTag(other); return a->equals(*b);
}
static_assert(std::is_same_v<decltype(&Inventory::$add),bool (Inventory::*)(ItemStack&)>);
static_assert(std::is_same_v<decltype(&Inventory::$canAdd),bool (Inventory::*)(ItemStack const&) const>);
LL_TYPE_INSTANCE_HOOK(AmmoCanAdd,ll::memory::HookPriority::Normal,Inventory,&Inventory::$canAdd,bool,ItemStack const& item) {
    int limit=limitFor(item); if (!limit) return origin(item);
    int accepted=int(item.mCount)-insertionPlan(*this,item,limit).remaining;
    auto native=origin(item); // read-only diagnostic comparison, not mutation
    if (inventorySamples.fetch_add(1)<40) TaczMod::getInstance().getSelf().getLogger().info(
        "[ammo-stack] canAdd id={} count={} limit={} accepted={} native={} adapter={}",readAmmoId(item)->toString(),item.mCount,limit,accepted,native,accepted>0);
    return accepted>0;
}
LL_TYPE_INSTANCE_HOOK(AmmoAdd,ll::memory::HookPriority::Normal,Inventory,&Inventory::$add,bool,ItemStack& item) {
    int limit=limitFor(item); if (!limit) return origin(item);
    auto id=readAmmoId(item)->toString(); int requested=item.mCount;
    int inserted=insertAmmo(*this,item,limit);
    if (inventorySamples.fetch_add(1)<40) TaczMod::getInstance().getSelf().getLogger().info(
        "[ammo-stack] add id={} requested={} inserted={} remaining={} limit={}",id,requested,inserted,item.mCount,limit);
    return inserted>0;
}
bool addHook=false,canAddHook=false,sameDataHook=false;
}
void enableAmmoInventory() {
    addHook=AmmoAdd::hook()==0; canAddHook=AmmoCanAdd::hook()==0;
    sameDataHook=AmmoSameData::hook()==0;
    TaczMod::getInstance().getSelf().getLogger().info("[ammo-stack] addHook={} canAddHook={} sameDataHook={}",addHook,canAddHook,sameDataHook);
}
void disableAmmoInventory() {
    if (addHook) AmmoAdd::unhook(); if (canAddHook) AmmoCanAdd::unhook();
    if (sameDataHook) AmmoSameData::unhook();
    addHook=canAddHook=sameDataHook=false; inventorySamples=0;
}

std::optional<pack::ResourceId> readAmmoId(ItemStackBase const& item) {
    if (item.isNull()) return std::nullopt;
    auto const& tag = item.mUserData;
    if (!tag || !tag->contains(AmmoId, Tag::String)) return std::nullopt;
    return pack::ResourceId::parse(static_cast<std::string const&>((*tag)[AmmoId]));
}

int countAmmo(Player const& player, pack::ResourceId const& ammoId) {
    auto& inventory = player.getInventory();
    int   total     = 0;
    for (int slot = 0; slot < inventory.getContainerSize(); ++slot) {
        auto const& item = inventory.getItem(slot);
        if (readAmmoId(item) == ammoId) total += item.mCount;
    }
    return total;
}

int extractAmmo(Player& player, pack::ResourceId const& ammoId, int needed) {
    if (needed <= 0) return 0;
    auto& inventory = player.getInventory();
    int   taken     = 0;
    for (int slot = 0; slot < inventory.getContainerSize() && taken < needed; ++slot) {
        auto const& item = inventory.getItem(slot);
        if (readAmmoId(item) != ammoId) continue;
        int amount = std::min<int>(item.mCount, needed - taken);
        inventory.removeItem(slot, amount);
        taken += amount;
    }
    if (taken > 0) player.sendInventory(false);
    return taken;
}

int giveAmmo(Player& player, pack::ResourceId const& ammoId, int count) {
    auto const& packs=TaczMod::getInstance().gunPacks();
    auto ammo=packs.ammo(ammoId); if (!ammo || count<=0) return 0;
    auto& inventory=player.getInventory();
    int limit=pack::ammoStackLimit(ammo->index.stackSize),given=0;
    for (int chunk:pack::splitAmmo(count,limit)) {
        ItemStack stack; stack.reinit("tacz:ammo",chunk,0);
        if (stack.isNull()) break;
        auto tag=std::make_unique<CompoundTag>(); (*tag)[AmmoId]=StringTag{ammoId.toString()};
        stack.setUserData(std::move(tag));
        int inserted=insertAmmo(inventory,stack,limit); given+=inserted;
        if (inserted<chunk) break; // full; unissued rounds are never reported as given
    }
    if (given) player.refreshInventory();
    TaczMod::getInstance().getSelf().getLogger().info("[ammo-stack] give id={} requested={} inserted={} limit={}",ammoId.toString(),count,given,limit);
    return given;
}

} // namespace tacz::nbt
