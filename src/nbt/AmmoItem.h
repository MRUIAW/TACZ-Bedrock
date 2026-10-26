#pragma once

#include <optional>

#include "pack/ResourceId.h"

class ItemStack;
class ItemStackBase;
class Player;

// Ammo items, identified like TACZ's by the `AmmoId` NBT key (AmmoItemDataAccessor). There is
// tacz:ammo is the registered base item; legacy stacks carrying AmmoId remain readable.
namespace tacz::nbt {

std::optional<pack::ResourceId> readAmmoId(ItemStackBase const& item);

// Rounds of `ammoId` in the player's inventory.
int countAmmo(Player const& player, pack::ResourceId const& ammoId);

// AbstractGunItem.findAndExtractInventoryAmmo: takes up to `needed` rounds from the inventory,
// slot by slot. Returns how many were taken and syncs the inventory if any were.
int extractAmmo(Player& player, pack::ResourceId const& ammoId, int needed);

// Gives per-ammo-sized stacks; drops overflow like /give. Returns inserted+dropped.
int giveAmmo(Player& player, pack::ResourceId const& ammoId, int count);
void enableAmmoInventory();
void disableAmmoInventory();

} // namespace tacz::nbt
