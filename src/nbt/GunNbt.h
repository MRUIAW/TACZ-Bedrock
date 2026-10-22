#pragma once

#include <string_view>

// Item NBT keys, kept identical to TACZ (com.tacz.guns.api.item.nbt) so that
// data written by either side reads the same way.
namespace tacz::nbt {

inline constexpr std::string_view GunId               = "GunId";
inline constexpr std::string_view GunDisplayId        = "GunDisplayId";
inline constexpr std::string_view GunFireMode         = "GunFireMode";
inline constexpr std::string_view GunCurrentAmmoCount = "GunCurrentAmmoCount";
inline constexpr std::string_view HasBulletInBarrel   = "HasBulletInBarrel";

} // namespace tacz::nbt
