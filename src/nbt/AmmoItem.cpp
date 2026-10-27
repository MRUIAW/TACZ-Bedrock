#include "nbt/AmmoItem.h"

#include <algorithm>
#include <memory>
#include <string>
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
#include "nbt/InventorySync.h"
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
// Command-only /tacz give plan. Native inventory, prediction and pickup are not
// redirected here; those use the descriptor bridge and vanilla transactions.
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
int insertAmmo(Player& player,ItemStack& source,int limit) {
    auto& inventory=player.getInventory();
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
        copy.set(old+amount); inventory.setItemWithForceBalance(slot,copy,true);
        sendTrackedInventorySlot(player,slot,old,"give-ammo");
    }
    int inserted=int(source.mCount)-plan.remaining;
    source.set(plan.remaining); // leave uninserted rounds in the ground/caller stack
    return inserted;
}
static_assert(std::is_same_v<decltype(&ItemStackBase::hasSameUserData),bool (ItemStackBase::*)(ItemStackBase const&) const>);
LL_TYPE_INSTANCE_HOOK(AmmoSameData,ll::memory::HookPriority::Normal,ItemStackBase,&ItemStackBase::hasSameUserData,bool,ItemStackBase const& other) {
    auto marked=[](ItemStackBase const& stack) { return stack.mUserData && stack.mUserData->contains("TaczAutoName",Tag::String); };
    if (!marked(*this) && !marked(other)) return origin(other);
    if (!limitFor(*this) || !limitFor(other)) return origin(other);
    // Narrow migration compatibility ONLY. Removing this would prevent marked
    // old ammo and otherwise identical new ammo from merging. Native item/aux
    // checks and all meaningful NBT (AmmoId/custom names/lore) remain authoritative.
    auto a=canonicalTag(*this),b=canonicalTag(other); return a->equals(*b);
}
bool sameDataHook=false;
}
void enableAmmoInventory() {
    enableInventorySyncDiagnostics();
    sameDataHook=AmmoSameData::hook()==0;
    TaczMod::getInstance().getSelf().getLogger().info("[ammo-stack] nativeInventory=true nativePickup=true legacyAutoNameHook={}",sameDataHook);
}
void disableAmmoInventory() {
    disableInventorySyncDiagnostics();
    if (sameDataHook) AmmoSameData::unhook();
    sameDataHook=false;
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
        int before=item.mCount;
        // External server-authoritative consumption must not remain an ordinary
        // unbalanced inventory action that can coalesce with the next pickup.
        ItemStack remaining(item);
        remaining.remove(amount);
        inventory.setItemWithForceBalance(slot,remaining,true);
        sendTrackedInventorySlot(player,slot,before,"extract");
        taken += amount;
    }
    return taken;
}

int giveAmmo(Player& player, pack::ResourceId const& ammoId, int count) {
    auto const& packs=TaczMod::getInstance().gunPacks();
    auto ammo=packs.ammo(ammoId); if (!ammo || count<=0) return 0;
    int limit=pack::ammoStackLimit(ammo->index.stackSize),given=0,dropped=0;
    for (int chunk:pack::splitAmmo(count,limit)) {
        ItemStack stack; stack.reinit("tacz:ammo",chunk,0);
        if (stack.isNull()) break;
        auto tag=std::make_unique<CompoundTag>(); (*tag)[AmmoId]=StringTag{ammoId.toString()};
        stack.setUserData(std::move(tag));
        int inserted=insertAmmo(player,stack,limit); given+=inserted;
        if (!stack.isNull()) {
            int remaining=stack.mCount;
            if (!player.drop(stack,false)) break;
            dropped+=remaining;
        }
    }
    TaczMod::getInstance().getSelf().getLogger().info("[ammo-stack] give id={} requested={} inserted={} dropped={} limit={}",ammoId.toString(),count,given,dropped,limit);
    return given+dropped;
}

} // namespace tacz::nbt
