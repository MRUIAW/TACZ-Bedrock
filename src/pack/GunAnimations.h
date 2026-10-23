#pragma once

#include "model/Animation.h"
#include "pack/GunPackLoader.h"

namespace tacz::pack {

// GunDisplayInstance.checkAnimation: explicit animation first, defaults fill missing names.
animation::AnimationFile loadGunAnimations(
    GunPackRegistry const& registry,
    GunDisplay const& display,
    animation::LogFn const& warn = {}
);

} // namespace tacz::pack
