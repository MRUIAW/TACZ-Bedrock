#include "nbt/InventorySync.h"
#include <string_view>
#include <type_traits>
#include <vector>
#include "debug/AmmoSyncDebug.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/service/Bedrock.h"
#include "mc/world/item/ItemStack.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/item/ItemActor.h"
#include "mc/world/item/RangedWeaponItem.h"
#include "mc/network/packet/InventorySlotPacket.h"
#include "mc/network/packet/InventoryContentPacket.h"
#include "mc/world/inventory/transaction/InventoryAction.h"
#include "mod/TaczMod.h"
#include "nbt/AmmoItem.h"
#include "nbt/GunItem.h"
#include "model/InventorySlotSync.h"
#ifdef TACZ_CLIENT
#include "client/gameplay/ClientShooter.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/network/packet/InventoryTransactionPacket.h"
#endif

namespace tacz::nbt {
namespace {
auto& logger() { return TaczMod::getInstance().getSelf().getLogger(); }
thread_local char const* syncReason="native";
struct Reason {
    char const* previous=syncReason;
    explicit Reason(char const* value) { syncReason=value; }
    ~Reason() { syncReason=previous; }
};
InventoryMenu* inventoryMenu(Player& player) {
    if (!ll::service::getLevel() || player.isClientSide()) return nullptr;
    auto server=ServerPlayer::tryGetFromEntity(player.getEntityContext(),false);
    if (!server) return nullptr;
    auto& menu=server->mInventoryMenu.get();
    return menu._getContainer()==&player.getInventory() ? &menu : nullptr;
}
int baselineCount(InventoryMenu& menu,int slot) {
    auto const& slots=menu.mLastSlots.get();
    return slot>=0 && size_t(slot)<slots.size() ? int(slots[slot].mCount) : -1;
}
bool relevant(ItemStack const& item) { return readAmmoId(item).has_value() || readGun(item).has_value(); }
using SendPacket=void (ServerPlayer::*)(Packet&) const;
static_assert(std::is_same_v<decltype(&ServerPlayer::$sendNetworkPacket),SendPacket>);
LL_TYPE_INSTANCE_HOOK(AmmoSyncSend,ll::memory::HookPriority::Normal,ServerPlayer,&ServerPlayer::$sendNetworkPacket,void,Packet& packet) {
    if (debug::ammoSync && ll::service::getLevel() && !isClientSide()) {
        if (packet.getId()==MinecraftPacketIds::InventorySlot) {
            auto const& p=static_cast<InventorySlotPacket const&>(packet);
            if (p.mInventoryId==ContainerID::Inventory && debug::ammoPacketSamples.take())
                logger().info("[ammo-sync] server path=sendNetworkPacket/InventorySlotPacket reason={} slot={} sent={}",syncReason,p.mSlot,p.mItem->mStackSize);
        } else if (packet.getId()==MinecraftPacketIds::InventoryContent) {
            auto const& p=static_cast<InventoryContentPacket const&>(packet);
            if (p.mInventoryId==ContainerID::Inventory) for (size_t slot=0;slot<p.mSlots->size();++slot)
                if (debug::ammoPacketSamples.take()) logger().info("[ammo-sync] server path=sendNetworkPacket/InventoryContentPacket reason={} slot={} sent={}",syncReason,slot,p.mSlots.get()[slot].mStackSize);
        } else if (packet.getId()==MinecraftPacketIds::InventoryTransaction && debug::ammoPacketSamples.take()) {
            logger().info("[ammo-sync] server path=sendNetworkPacket/InventoryTransactionPacket reason={}",syncReason);
        }
    }
    origin(packet);
}
using AddAction=void (InventoryTransactionManager::*)(InventoryAction const&,bool);
static_assert(std::is_same_v<decltype(&InventoryTransactionManager::addAction),AddAction>);
static_assert(std::is_same_v<decltype(&Inventory::setItemWithForceBalance),void (Inventory::*)(int,ItemStack const&,bool)>);
LL_TYPE_INSTANCE_HOOK(AmmoSyncAction,ll::memory::HookPriority::Normal,InventoryTransactionManager,&InventoryTransactionManager::addAction,void,
    InventoryAction const& action,bool forceBalanced) {
    if (!debug::ammoSync || !ll::service::getLevel()) return origin(action,forceBalanced);
    Player& player=mPlayer;
    bool trace=!player.isClientSide() && (relevant(action.mFromItem.get()) || relevant(action.mToItem.get()) || std::string_view(syncReason)=="vanilla-ranged")
        && debug::ammoServerSamples.take();
    int before=trace ? int(action.mFromItem->mCount) : 0,after=trace ? int(action.mToItem->mCount) : 0;
    auto slot=action.mSlot;
    bool pendingBefore=trace && bool(mCurrentTransaction);
    origin(action,forceBalanced);
    if (trace) logger().info("[ammo-sync] server path=InventoryTransactionManager::addAction reason={} slot={} before={} after={} forceBalanced={} pendingBefore={} pendingAfter={}",
        syncReason,slot,before,after,forceBalanced,pendingBefore,bool(mCurrentTransaction));
}
using SlotChanged=void (ServerPlayer::*)(IContainerManager&,Container&,int,ItemStack const&,ItemStack const&,bool);
static_assert(std::is_same_v<decltype(&ServerPlayer::$slotChanged),SlotChanged>);
static_assert(std::is_same_v<decltype(&BaseContainerMenu::broadcastChanges),void (BaseContainerMenu::*)()>);
static_assert(std::is_same_v<decltype(&ItemStack::operator=),ItemStack& (ItemStack::*)(ItemStack const&)>);
LL_TYPE_INSTANCE_HOOK(AmmoSyncSlotChanged,ll::memory::HookPriority::Normal,ServerPlayer,&ServerPlayer::$slotChanged,void,
    IContainerManager& menu,Container& container,int slot,ItemStack const& oldItem,ItemStack const& newItem,bool result) {
    // Snapshot before origin; it may change the live stack/IDs. No retained refs.
    bool trace=debug::ammoSync && ll::service::getLevel() && !isClientSide()
        && (relevant(oldItem) || relevant(newItem)) && debug::ammoServerSamples.take();
    int before=trace ? int(oldItem.mCount) : 0,after=trace ? int(newItem.mCount) : 0;
    origin(menu,container,slot,oldItem,newItem,result);
    if (trace) logger().info("[ammo-sync] server path=slotChanged reason={} container={} slot={} before={} after={} ownMenuSuppressed={}",
        syncReason,int(menu.getContainerId()),slot,before,after,&menu==static_cast<IContainerManager*>(&mInventoryMenu.get()));
}
static_assert(std::is_same_v<decltype(&Player::take),bool (Player::*)(Actor&,int,int)>);
LL_TYPE_INSTANCE_HOOK(AmmoSyncPickup,ll::memory::HookPriority::Normal,Player,&Player::take,bool,Actor& actor,int count,int favoredSlot) {
    if (!debug::ammoSync || !debug::ammoPickupSamples.available() || !ll::service::getLevel() || isClientSide()) return origin(actor,count,favoredSlot);
    auto ground=ItemActor::tryGetFromEntity(actor.getEntityContext(),false);
    auto id=ground ? readAmmoId(ground->mItem.get()) : std::nullopt;
    if (!id) return origin(actor,count,favoredSlot);
    auto before=countAmmo(*this,*id);
    std::vector<int> counts;
    auto& inventory=getInventory();
    for (int slot=0;slot<inventory.getContainerSize();++slot) counts.push_back(inventory.getItem(slot).mCount);
    Reason reason("pickup");
    auto result=origin(actor,count,favoredSlot);
    auto after=countAmmo(*this,*id);
    if (before!=after) {
        auto menu=inventoryMenu(*this);
        for (int slot=0;slot<int(counts.size());++slot)
            if (counts[slot]!=inventory.getItem(slot).mCount && readAmmoId(inventory.getItem(slot))==id && debug::ammoPickupSamples.take())
                logger().info("[ammo-sync] server path=Player::take/native reason=pickup ammo={} slot={} before={} after={} totalBefore={} totalAfter={} baselineAfter={} nativeResult={}",
                    id->toString(),slot,counts[slot],int(inventory.getItem(slot).mCount),before,after,menu ? baselineCount(*menu,slot) : -1,result);
    }
    return result;
}
// Optional control experiment: native ranged consumption is not replaced. This
// records the before/after menu baseline for bow/crossbow comparisons in-game.
static_assert(std::is_same_v<decltype(&RangedWeaponItem::$releaseUsing),void (RangedWeaponItem::*)(ItemStack&,Player*,int) const>);
LL_TYPE_INSTANCE_HOOK(AmmoSyncRanged,ll::memory::HookPriority::Normal,RangedWeaponItem,&RangedWeaponItem::$releaseUsing,void,
    ItemStack& item,Player* player,int duration) {
    if (!debug::ammoSync || !debug::ammoServerSamples.available() || !player || !ll::service::getLevel() || player->isClientSide()) return origin(item,player,duration);
    auto menu=inventoryMenu(*player);
    auto const& inv=player->getInventory();
    std::vector<int> counts;
    for (int slot=0;slot<inv.getContainerSize();++slot) counts.push_back(inv.getItem(slot).mCount);
    Reason reason("vanilla-ranged");
    origin(item,player,duration);
    for (int slot=0;slot<int(counts.size());++slot) if (counts[slot]!=inv.getItem(slot).mCount && debug::ammoServerSamples.take())
        logger().info("[ammo-sync] server path=RangedWeaponItem::releaseUsing/native slot={} before={} after={} baselineAfter={}",
            slot,counts[slot],int(inv.getItem(slot).mCount),menu ? baselineCount(*menu,slot) : -1);
}
#ifdef TACZ_CLIENT
using ReceiveSlot=void (LegacyClientNetworkHandler::*)(NetworkIdentifier const&,InventorySlotPacket const&);
const auto receiveSlot=static_cast<ReceiveSlot>(&LegacyClientNetworkHandler::$handle);
static_assert(std::is_same_v<std::remove_cv_t<decltype(receiveSlot)>,ReceiveSlot>);
LL_TYPE_INSTANCE_HOOK(AmmoSyncReceive,ll::memory::HookPriority::Normal,LegacyClientNetworkHandler,receiveSlot,void,
    NetworkIdentifier const& source,InventorySlotPacket const& packet) {
    if (!debug::ammoSync || !client::shooter::worldToken()) return origin(source,packet);
    IClientInstance& client=mClient;
    bool trace=debug::ammoSync && client::shooter::worldToken() && client::shooter::ownsClient(&client)
        && packet.mInventoryId==ContainerID::Inventory && debug::ammoClientSamples.take();
    int before=-1;
    auto slot=packet.mSlot;
    if (trace) if (auto player=client.getLocalPlayer(); player && slot<uint(player->getInventory().getContainerSize()))
        before=player->getInventory().getItem(slot).mCount;
    origin(source,packet);
    if (trace) {
        int after=-1;
        if (auto player=client.getLocalPlayer(); player && slot<uint(player->getInventory().getContainerSize()))
            after=player->getInventory().getItem(slot).mCount;
        logger().info("[ammo-sync] client path=InventorySlotPacket/handle slot={} before={} received={} after={}",
            slot,before,packet.mItem->mStackSize,after);
    }
}
bool receiveHook=false;
using ReceiveContent=void (LegacyClientNetworkHandler::*)(NetworkIdentifier const&,InventoryContentPacket const&);
const auto receiveContent=static_cast<ReceiveContent>(&LegacyClientNetworkHandler::$handle);
static_assert(std::is_same_v<std::remove_cv_t<decltype(receiveContent)>,ReceiveContent>);
LL_TYPE_INSTANCE_HOOK(AmmoSyncReceiveContent,ll::memory::HookPriority::Normal,LegacyClientNetworkHandler,receiveContent,void,
    NetworkIdentifier const& source,InventoryContentPacket const& packet) {
    if (!debug::ammoSync || !client::shooter::worldToken() || !debug::ammoClientSamples.available()) return origin(source,packet);
    IClientInstance& client=mClient;
    if (!client::shooter::ownsClient(&client) || packet.mInventoryId!=ContainerID::Inventory) return origin(source,packet);
    std::vector<int> before;
    if (auto player=client.getLocalPlayer()) for (int slot=0;slot<player->getInventory().getContainerSize();++slot)
        before.push_back(player->getInventory().getItem(slot).mCount);
    origin(source,packet);
    auto player=client.getLocalPlayer();
    for (size_t slot=0;slot<packet.mSlots->size() && slot<before.size();++slot) if (debug::ammoClientSamples.take())
        logger().info("[ammo-sync] client path=InventoryContentPacket/handle slot={} before={} received={} after={}",slot,before[slot],packet.mSlots.get()[slot].mStackSize,
            player && slot<size_t(player->getInventory().getContainerSize()) ? int(player->getInventory().getItem(int(slot)).mCount) : -1);
}
using ReceiveTransaction=void (LegacyClientNetworkHandler::*)(NetworkIdentifier const&,std::shared_ptr<InventoryTransactionPacket>);
const auto receiveTransaction=static_cast<ReceiveTransaction>(&LegacyClientNetworkHandler::$handle);
static_assert(std::is_same_v<std::remove_cv_t<decltype(receiveTransaction)>,ReceiveTransaction>);
LL_TYPE_INSTANCE_HOOK(AmmoSyncReceiveTransaction,ll::memory::HookPriority::Normal,LegacyClientNetworkHandler,receiveTransaction,void,
    NetworkIdentifier const& source,std::shared_ptr<InventoryTransactionPacket> packet) {
    if (!debug::ammoSync || !client::shooter::worldToken() || !debug::ammoClientSamples.available()) return origin(source,std::move(packet));
    IClientInstance& client=mClient;
    if (!client::shooter::ownsClient(&client)) return origin(source,std::move(packet));
    std::vector<int> before;
    if (auto player=client.getLocalPlayer()) for (int slot=0;slot<player->getInventory().getContainerSize();++slot)
        before.push_back(player->getInventory().getItem(slot).mCount);
    origin(source,std::move(packet));
    auto player=client.getLocalPlayer();
    if (debug::ammoClientSamples.take()) logger().info("[ammo-sync] client path=InventoryTransactionPacket/handle received=true");
    if (player) for (int slot=0;slot<int(before.size()) && slot<player->getInventory().getContainerSize();++slot) {
        int after=player->getInventory().getItem(slot).mCount;
        if (before[slot]!=after && debug::ammoClientSamples.take())
            logger().info("[ammo-sync] client path=InventoryTransactionPacket/applied slot={} before={} after={}",slot,before[slot],after);
    }
}
bool contentHook=false,transactionHook=false;
#endif
bool slotHook=false,pickupHook=false,rangedHook=false,sendHook=false,actionHook=false;
}
void sendTrackedInventorySlot(Player& player,int slot,int before,char const* reason) {
    if (!inventoryMenu(player) || slot<0 || slot>=player.getInventory().getContainerSize()) return;
    Reason scope(reason);
    InventorySlotPacket packet{InventorySlotPacketPayload{ContainerID::Inventory,slot,player.getInventory().getItem(slot)}};
    player.sendNetworkPacket(packet);
    recordSentInventorySlot(player,slot,before,reason);
}
void recordSentInventorySlot(Player& player,int slot,int before,char const* reason) {
    auto menu=inventoryMenu(player); if (!menu) return;
    auto& slots=menu->mLastSlots.get();
    // Never initialize/resize the whole native baseline from a one-slot send:
    // unsent slots might contain transient values. Native owns initial setup.
    int last=baselineCount(*menu,slot);
    if (slot<0 || size_t(slot)>=slots.size()) {
        if (debug::ammoSync && debug::ammoServerSamples.take())
            logger().info("[ammo-sync] server path=explicit-InventorySlotPacket reason={} slot={} baseline=not-initialized",reason,slot);
        return;
    }
    auto const& current=player.getInventory().getItem(slot);
    model::commitSentInventorySlot(slots,slot,current);
    if (debug::ammoSync && debug::ammoServerSamples.take())
        logger().info("[ammo-sync] server path=explicit-InventorySlotPacket+baseline reason={} slot={} before={} after={} baselineBefore={} baselineAfter={}",
            reason,slot,before,int(current.mCount),last,baselineCount(*menu,slot));
}
void enableInventorySyncDiagnostics() {
    slotHook=AmmoSyncSlotChanged::hook()==0;
    pickupHook=AmmoSyncPickup::hook()==0;
    rangedHook=AmmoSyncRanged::hook()==0;
    sendHook=AmmoSyncSend::hook()==0;
    actionHook=AmmoSyncAction::hook()==0;
#ifdef TACZ_CLIENT
    receiveHook=AmmoSyncReceive::hook()==0;
    contentHook=AmmoSyncReceiveContent::hook()==0;
    transactionHook=AmmoSyncReceiveTransaction::hook()==0;
    logger().info("[ammo-sync] client install slot={} content={} transaction={}",receiveHook,contentHook,transactionHook);
#endif
    logger().info("[ammo-sync] install slotChanged={} pickup={} vanillaRanged={} packetSend={} action={} default=off cap=256/path",slotHook,pickupHook,rangedHook,sendHook,actionHook);
}
void disableInventorySyncDiagnostics() {
    debug::setAmmoSync(false);
    if (slotHook) AmmoSyncSlotChanged::unhook(); slotHook=false;
    if (pickupHook) AmmoSyncPickup::unhook(); pickupHook=false;
    if (rangedHook) AmmoSyncRanged::unhook(); rangedHook=false;
    if (sendHook) AmmoSyncSend::unhook(); sendHook=false;
    if (actionHook) AmmoSyncAction::unhook(); actionHook=false;
#ifdef TACZ_CLIENT
    if (receiveHook) AmmoSyncReceive::unhook(); receiveHook=false;
    if (contentHook) AmmoSyncReceiveContent::unhook(); contentHook=false;
    if (transactionHook) AmmoSyncReceiveTransaction::unhook(); transactionHook=false;
#endif
}
}
