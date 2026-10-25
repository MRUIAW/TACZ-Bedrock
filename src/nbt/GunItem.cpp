#include "nbt/GunItem.h"

#include <memory>

#include "mc/deps/nbt/ByteTag.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include "nbt/GunNbt.h"
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

bool writeSelectedGun(Player& player, GunItemState const& state) {
    ItemStack item(player.getSelectedItem());
    if (item.isNull()) return false;
    auto tag = item.mUserData ? item.mUserData->clone() : std::make_unique<CompoundTag>();
    (*tag)[GunId]               = StringTag{state.gunId.toString()};
    (*tag)[GunFireMode]         = StringTag{fireModeName(state.fireMode)};
    (*tag)[GunCurrentAmmoCount] = IntTag{state.ammo};
    (*tag)[HasBulletInBarrel]   = ByteTag{state.barrel ? 1 : 0};
    item.setUserData(std::move(tag));
    player.setSelectedItem(item);
    player.sendInventory(false);
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
