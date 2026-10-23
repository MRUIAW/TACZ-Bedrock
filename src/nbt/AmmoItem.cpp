#include "nbt/AmmoItem.h"

#include <algorithm>
#include <memory>
#include <string>

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/Container.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include "nbt/GunNbt.h"

namespace tacz::nbt {

std::optional<pack::ResourceId> readAmmoId(ItemStack const& item) {
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

bool giveAmmo(Player& player, pack::ResourceId const& ammoId, int count) {
    ItemStack stack;
    stack.reinit("minecraft:iron_nugget", std::clamp(count, 1, 64), 0);
    if (stack.isNull()) return false;
    auto tag      = std::make_unique<CompoundTag>();
    (*tag)[AmmoId] = StringTag{ammoId.toString()};
    stack.setUserData(std::move(tag));
    return player.addAndRefresh(stack);
}

} // namespace tacz::nbt
