#pragma once

#include <filesystem>

#include "ll/api/mod/NativeMod.h"

#include "pack/GunPackLoader.h"

namespace tacz {

class TaczMod {

public:
    static TaczMod& getInstance();

    TaczMod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    /// @return True if the mod is loaded successfully.
    bool load();

    /// @return True if the mod is enabled successfully.
    bool enable();

    /// @return True if the mod is disabled successfully.
    bool disable();

    /// Folder scanned for gun packs, the counterpart of Java's `.minecraft/tacz`.
    [[nodiscard]] std::filesystem::path gunPackDir() const { return mSelf.getModDir() / "gunpacks"; }

    [[nodiscard]] pack::GunPackRegistry const& gunPacks() const { return mGunPacks; }

    void reloadGunPacks();

private:
    ll::mod::NativeMod&   mSelf;
    pack::GunPackRegistry mGunPacks;
};

} // namespace tacz
