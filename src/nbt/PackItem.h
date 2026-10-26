#pragma once
#include "pack/PackItems.h"
class ItemStack;
class ItemStackBase;
class Player;
namespace tacz::nbt {
std::optional<pack::ItemIdentity> readItemIdentity(ItemStackBase const& item);
std::optional<pack::ResourceId> readAttachmentId(ItemStackBase const& item);
void writeAttachmentId(ItemStack& item,pack::ResourceId const& id);
bool giveGun(Player& player,pack::ResourceId const& id);
void enablePackItems();
void disablePackItems();
}
