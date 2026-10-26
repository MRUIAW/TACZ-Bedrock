#pragma once

#include <optional>

#include "pack/ResourceId.h"

class ItemStack;
class Player;

// Ammo items, identified like TACZ's by the `AmmoId` NBT key (AmmoItemDataAccessor). There is
// no dedicated ammo item yet, so any item stack carrying AmmoId counts as ammo.
namespace tacz::nbt {

std::optional<pack::ResourceId> readAmmoId(ItemStack const& item);

// Rounds of `ammoId` in the player's inventory.
int countAmmo(Player const& player, pack::ResourceId const& ammoId);

// AbstractGunItem.findAndExtractInventoryAmmo: takes up to `needed` rounds from the inventory,
// slot by slot. Returns how many were taken and syncs the inventory if any were.
int extractAmmo(Player& player, pack::ResourceId const& ammoId, int needed);

// Gives a stack of ammo (placeholder base item: iron nugget). False if the inventory is full.
int giveAmmo(Player& player, pack::ResourceId const& ammoId, int count);

} // namespace tacz::nbt
