#include "mod/TaczMod.h"

#include "ll/api/mod/RegisterHelper.h"

#include "server/BulletSystem.h"
#include "server/ServerProbe.h"
#include "server/Shooter.h"
#include "server/HitInput.h"
#include "nbt/PackItem.h"
#include "nbt/StackLimitProbe.h"
#ifdef TACZ_CLIENT
#include "client/ClientProbe.h"
#include "client/BulletExperiment.h"
#include "client/gameplay/ClientShooter.h"
#include "client/render/GunRenderer.h"
#include "client/render/SkinTextureSource.h"
#include "client/ui/GunHud.h"
#endif

namespace tacz {

TaczMod& TaczMod::getInstance() {
    static TaczMod instance;
    return instance;
}

bool TaczMod::load() {
#ifdef TACZ_CLIENT
    getSelf().getLogger().info("Loading TACZ {} (client)...", TACZ_VERSION);
    // Capture weak native skin identities before the dressing room / world initializes.
    // This observer never queries client/world services or uploads anything at the menu.
    client::render::enableSkinTextureSource();
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
    nbt::enablePackItems();
    nbt::enableStackLimitProbe();
    server::probe::enable();
    server::hitinput::enable();
    server::bullet::enable();
    server::shooter::enable();
#ifdef TACZ_CLIENT
    client::probe::enable();
    client::shooter::enable();
    client::bullet::enable();
    client::render::enable();
    client::hud::enable();
#endif
    return true;
}

bool TaczMod::disable() {
    getSelf().getLogger().debug("Disabling...");
    nbt::disableStackLimitProbe();
    nbt::disablePackItems();
#ifdef TACZ_CLIENT
    client::hud::disable();
    client::render::disable();
    client::shooter::disable();
    client::bullet::disable();
    client::probe::disable();
#endif
    server::shooter::disable();
    server::hitinput::disable();
    server::bullet::disable();
    server::probe::disable();
    return true;
}

} // namespace tacz

LL_REGISTER_MOD(tacz::TaczMod, tacz::TaczMod::getInstance());
