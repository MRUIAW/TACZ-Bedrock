#include "mod/TaczMod.h"

#include "ll/api/mod/RegisterHelper.h"

#include "server/BulletSystem.h"
#include "server/ServerProbe.h"
#ifdef TACZ_CLIENT
#include "client/ClientProbe.h"
#include "client/render/GunRenderer.h"
#endif

namespace tacz {

TaczMod& TaczMod::getInstance() {
    static TaczMod instance;
    return instance;
}

bool TaczMod::load() {
#ifdef TACZ_CLIENT
    getSelf().getLogger().info("Loading TACZ {} (client)...", TACZ_VERSION);
#else
    getSelf().getLogger().info("Loading TACZ {} (server)...", TACZ_VERSION);
#endif
    reloadGunPacks();
    return true;
}

void TaczMod::reloadGunPacks() {
    auto& logger = getSelf().getLogger();
    mGunPacks.loadFolder(
        gunPackDir(),
        [&](std::string const& msg) { logger.info("{}", msg); },
        [&](std::string const& msg) { logger.warn("{}", msg); }
    );
}

bool TaczMod::enable() {
    getSelf().getLogger().debug("Enabling...");
    server::probe::enable();
    server::bullet::enable();
#ifdef TACZ_CLIENT
    client::probe::enable();
    client::render::enable();
#endif
    return true;
}

bool TaczMod::disable() {
    getSelf().getLogger().debug("Disabling...");
#ifdef TACZ_CLIENT
    client::render::disable();
    client::probe::disable();
#endif
    server::bullet::disable();
    server::probe::disable();
    return true;
}

} // namespace tacz

LL_REGISTER_MOD(tacz::TaczMod, tacz::TaczMod::getInstance());
