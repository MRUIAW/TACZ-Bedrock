#pragma once

#include <optional>
#include <string>

#include "pack/GunData.h"
#include "pack/ResourceId.h"

class ItemStack;
class Player;

// Gun state stored on the item, read and written with TACZ's NBT keys and encodings
// (api/item/nbt/GunItemDataAccessor): GunFireMode is the Java enum name, HasBulletInBarrel a byte.
namespace tacz::nbt {

struct GunItemState {
    pack::ResourceId              gunId;
    std::optional<pack::FireMode> fireMode; // nullopt = TACZ FireMode.UNKNOWN
    int                           ammo   = 0;
    bool                          barrel = false;
};

// "AUTO" / "SEMI" / "BURST" / "UNKNOWN".
std::string                   fireModeName(std::optional<pack::FireMode> mode);
std::optional<pack::FireMode> parseFireModeName(std::string const& name);

// The gun state of an item, or nullopt if it has no valid GunId.
std::optional<GunItemState> readGun(ItemStack const& item);

// Writes the state into the player's selected slot and syncs the inventory to the client.
// Returns false if the slot is empty.
bool writeSelectedGun(Player& player, GunItemState const& state);

// TACZ AbstractGunItem.fillItemCategory: first fire mode, full magazine, round chambered.
GunItemState freshGun(pack::ResourceId const& gunId, pack::GunData const& data);

} // namespace tacz::nbt
