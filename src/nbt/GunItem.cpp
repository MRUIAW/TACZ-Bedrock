#include "nbt/GunItem.h"

#include <memory>

#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/network/packet/InventorySlotPacket.h"
#include "mc/world/ContainerID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/item/ItemStack.h"

#include "nbt/GunNbt.h"
#include "nbt/InventorySync.h"
#include "model/InventorySlotSync.h"
#include "gun/ShootRules.h"

namespace tacz::nbt {

std::string fireModeName(std::optional<pack::FireMode> mode) {
    if (!mode) return "UNKNOWN";
    switch (*mode) {
    case pack::FireMode::Auto:
        return "AUTO";
    case pack::FireMode::Semi:
        return "SEMI";
    case pack::FireMode::Burst:
        return "BURST";
    }
    return "UNKNOWN";
}

std::optional<pack::FireMode> parseFireModeName(std::string const& name) {
    if (name == "AUTO") return pack::FireMode::Auto;
    if (name == "SEMI") return pack::FireMode::Semi;
    if (name == "BURST") return pack::FireMode::Burst;
    return std::nullopt;
}

std::optional<GunItemState> readGun(ItemStack const& item) {
    auto const& tag = item.mUserData;
    if (!tag || !tag->contains(GunId, Tag::String)) return std::nullopt;
    auto id = pack::ResourceId::parse(static_cast<std::string const&>((*tag)[GunId]));
    if (!id) return std::nullopt;
    GunItemState state{*id};
    if (tag->contains(GunFireMode, Tag::String)) {
        state.fireMode = parseFireModeName(static_cast<std::string const&>((*tag)[GunFireMode]));
    }
    if (tag->contains(GunCurrentAmmoCount, Tag::Int)) state.ammo = static_cast<int>((*tag)[GunCurrentAmmoCount]);
    if (tag->contains(HasBulletInBarrel, Tag::Byte)) state.barrel = static_cast<int>((*tag)[HasBulletInBarrel]) != 0;
    return state;
}

nlohmann::json readNbt(CompoundTag const* tag, std::vector<std::string> const& path, std::string const& key, int mode) {
    if (path.size() > 64) return {};
    for (auto const& p : path) {
        if (!tag || !tag->contains(p, Tag::Compound)) return {};
        tag = &(*tag)[p].get<CompoundTag>();
    }
    if (!tag || !tag->contains(key)) return {};
    auto const& value = (*tag)[key];
    if (mode == 0) return static_cast<int>(value.index());
    switch (static_cast<Tag::Type>(value.index())) {
    case Tag::Byte: return static_cast<schar>(value.get<ByteTag>().data);
    case Tag::Short: return value.get<ShortTag>().data;
    case Tag::Int: return value.get<IntTag>().data;
    case Tag::Int64: return value.get<Int64Tag>().data;
    case Tag::Float: return value.get<FloatTag>().data;
    case Tag::Double: return value.get<DoubleTag>().data;
    case Tag::String: return static_cast<std::string const&>(value.get<StringTag>());
    default: return {};
    }
}

bool writeSelectedGun(Player& player, GunItemState const& state) {
    ItemStack item(player.getSelectedItem());
    if (item.isNull()) return false;
    int before=item.mCount;
    auto tag = item.mUserData ? item.mUserData->clone() : std::make_unique<CompoundTag>();
    (*tag)[GunId]               = StringTag{state.gunId.toString()};
    (*tag)[GunFireMode]         = StringTag{fireModeName(state.fireMode)};
    (*tag)[GunCurrentAmmoCount] = IntTag{state.ammo};
    (*tag)[HasBulletInBarrel]   = ByteTag{state.barrel ? 1 : 0};
    item.setUserData(std::move(tag));
    // NBT is part of the native transaction balance key. A plain
    // setSelectedItem -> Inventory::setItem leaves old-gun -1 / new-gun +1
    // pending even though both counts are 1, blocking the next pickup's otherwise
    // balanced actions. Finish this server-side rewrite through the native API.
    model::writeServerInventorySlot(player.getInventory(),player.getSelectedItemSlot(),item);
    // Keep the user-verified immediate NBT-only single-slot update. Commit only
    // its corresponding native baseline, never other slots' intermediate counts.
    InventorySlotPacket packet{InventorySlotPacketPayload{ContainerID::Inventory,player.getSelectedItemSlot(),player.getSelectedItem()}};
    player.sendNetworkPacket(packet);
    recordSentInventorySlot(player,player.getSelectedItemSlot(),before,"gun-nbt");
    return true;
}

GunItemState freshGun(pack::ResourceId const& gunId, pack::GunData const& data) {
    GunItemState state{gunId};
    state.fireMode = data.fireModes.empty() ? std::nullopt : data.fireModes.front();
    auto ammo=gun::initialAmmo(data);
    state.ammo     = ammo.magazine;
    state.barrel   = ammo.barrel;
    return state;
}

} // namespace tacz::nbt
