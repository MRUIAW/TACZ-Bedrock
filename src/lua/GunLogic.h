#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "gun/Reload.h"
#include "gun/CycleTasks.h"
#include "lua/ScriptApi.h"
#include "pack/GunPackLoader.h"

// Runs gun pack logic scripts (data/<ns>/scripts/*.lua) the way TACZ's ScriptManager and
// ModernKineticGunItem do: one Lua state per side, modules registered in package.preload as
// "<ns>_<path>", the gun's script picked by its `script` field, TACZ's constants as globals.
// A gun without a script, or a script without the function, falls back to TACZ's default logic.
//
// Sandbox: base, package, string, table and math only; no file access, no native modules,
// no binary chunks.
namespace tacz::lua {

class GunLogic {
public:
    using LogFn = std::function<void(std::string const&)>;

    GunLogic(pack::GunPackRegistry const& packs, LogFn warn, gun::CycleTasks::Clock clock = {});
    ~GunLogic();

    GunLogic(GunLogic const&)            = delete;
    GunLogic& operator=(GunLogic const&) = delete;

    // ModernKineticGunItem.startReload: false cancels the reload.
    bool startReload(ScriptApi& api);
    // ModernKineticGunItem.tickReload.
    gun::ReloadState tickReload(ScriptApi& api);
    // ModernKineticGunItem.interruptReload (no default behaviour).
    void interruptReload(ScriptApi& api);

    // ModernKineticGunItem.startBolt/tickBolt: independently default each missing hook.
    // Errors (including non-functions/non-booleans) warn once per gun/hook and stop.
    bool startBolt(ScriptApi& api);
    bool tickBolt(ScriptApi& api);

    void shoot(ScriptApi& api);
    void tickHeat(ScriptApi& api, int64_t heatTimestamp);
    void tickTasks(int64_t shooterKey);
    void clearTasks(int64_t shooterKey);
    void removeShooter(int64_t shooterKey);
    void cancelShots(int64_t shooterKey);
    [[nodiscard]] bool pendingShots(int64_t shooterKey) const;

    // ShooterDataHolder.scriptData: dropped when the shooter's gun state resets.
    void clearScriptData(int64_t shooterKey);

    // Loads a script module; false (and a warning) if it does not compile or return a table.
    bool loadScript(pack::ResourceId const& id);

    struct Impl;

private:
    std::unique_ptr<Impl> mImpl;
};

} // namespace tacz::lua
