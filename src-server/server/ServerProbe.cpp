#include "server/ServerProbe.h"

#include <algorithm>
#include <memory>
#include <string>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/io/Logger.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/Bedrock.h"

#include "mc/deps/nbt/CompoundTag.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/server/ServerPlayer.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include "debug/RenderDebug.h"
#include "mod/TaczMod.h"
#include "nbt/AmmoItem.h"
#include "nbt/GunItem.h"
#include "net/DebugPackets.h"

namespace tacz::server::probe {
namespace {

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

Player* originPlayer(CommandOrigin const& origin) {
    auto* entity = origin.getEntity();
    return entity && entity->isPlayer() ? static_cast<Player*>(entity) : nullptr;
}

} // namespace

// Command parameter structs need external linkage for LL's reflection (boost::pfr).
struct SetGunParams {
    std::string gunId;
};

struct RenderDebugParams {
    int material;
    int twoSided;
};

struct BulletDebugParams {
    int   moveMode;
    float speedScale;
};

struct AmmoParams {
    int count;
};

namespace {

void registerCommands() {
    auto& command = ll::command::CommandRegistrar::getServerInstance().getOrCreateCommand(
        "tacz",
        "TACZ debug command",
        CommandPermissionLevel::GameDirectors
    );
    // /tacz nbt : dump the selected item's user data
    command.overload().text("nbt").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* player = originPlayer(origin);
        if (!player) return output.error("Player only");
        auto const& tag  = player->getSelectedItem().mUserData;
        auto        snbt = tag ? tag->toSnbt(SnbtFormat::Minimize) : std::string{"<no user data>"};
        logger().info("[probe] nbt of {}'s held item: {}", player->getRealName(), snbt);
        // CommandOutput text does not show up in single-player chat; message the player directly.
        player->sendMessage("[TACZ] NBT: " + snbt);
        output.success("NBT: " + snbt);
    });
    // /tacz packs : summary of loaded gun packs
    command.overload().text("packs").execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto const& packs = TaczMod::getInstance().gunPacks();
        std::string msg   = "[TACZ] " + std::to_string(packs.packs().size()) + " pack(s), "
                        + std::to_string(packs.guns().size()) + " gun(s), " + std::to_string(packs.ammos().size())
                        + " ammo type(s)";
        if (auto* player = originPlayer(origin)) player->sendMessage(msg);
        output.success(msg);
    });
    // /tacz rdebug <material> <twoSided> : first-person gun render diagnostics
    command.overload<RenderDebugParams>()
        .text("rdebug")
        .required("material")
        .required("twoSided")
        .execute([](CommandOrigin const& origin, CommandOutput& output, RenderDebugParams const& params) {
            debug::gunMaterial = params.material;
            debug::gunTwoSided = params.twoSided != 0;
            debug::renderRevision++;
            std::string msg = "[TACZ] render debug: material=" + std::to_string(params.material)
                            + " twoSided=" + std::to_string(params.twoSided != 0);
            if (auto* player = originPlayer(origin)) player->sendMessage(msg);
            output.success(msg);
        });
    // /tacz bdebug <moveMode> <speedScale> : bullet entity diagnostics
    //   moveMode 0 = Actor::moveTo, 1 = Actor::teleport; speedScale < 1 slows bullets down to watch them.
    command.overload<BulletDebugParams>()
        .text("bdebug")
        .required("moveMode")
        .required("speedScale")
        .execute([](CommandOrigin const& origin, CommandOutput& output, BulletDebugParams const& params) {
            debug::bulletMoveMode   = params.moveMode;
            debug::bulletSpeedScale = params.speedScale > 0 ? params.speedScale : 1.0f;
            std::string msg         = "[TACZ] bullet debug: moveMode=" + std::to_string(params.moveMode)
                            + " speedScale=" + std::to_string(debug::bulletSpeedScale.load());
            if (auto* player = originPlayer(origin)) player->sendMessage(msg);
            output.success(msg);
        });
    // /tacz ammo <count> : give ammo for the held gun (an item carrying TACZ's AmmoId NBT; there is
    // no dedicated ammo item yet, so it looks like an iron nugget)
    command.overload<AmmoParams>()
        .text("ammo")
        .required("count")
        .execute([](CommandOrigin const& origin, CommandOutput& output, AmmoParams const& params) {
            auto* player = originPlayer(origin);
            if (!player) return output.error("Player only");
            auto gun   = nbt::readGun(player->getSelectedItem());
            auto entry = gun ? TaczMod::getInstance().gunPacks().gun(gun->gunId) : nullptr;
            if (!entry || !entry->data.ammoId) {
                player->sendMessage("[TACZ] Hold a gun first");
                return output.error("Hold a gun first");
            }
            int left = std::clamp(params.count, 1, 64 * 36);
            int given = 0;
            while (left > 0) {
                int stack = std::min(left, 64);
                if (!nbt::giveAmmo(*player, *entry->data.ammoId, stack)) break;
                given += stack;
                left -= stack;
            }
            player->sendMessage(
                "[TACZ] Gave " + std::to_string(given) + " x " + entry->data.ammoId->toString()
                + (left > 0 ? " (inventory full)" : "")
            );
        });
    // /tacz setgun <gunId> : turn the selected item into a gun as TACZ's creative tab hands it out
    // (first fire mode, full magazine, round chambered)
    command.overload<SetGunParams>()
        .text("setgun")
        .required("gunId")
        .execute([](CommandOrigin const& origin, CommandOutput& output, SetGunParams const& params) {
            auto* player = originPlayer(origin);
            if (!player) return output.error("Player only");
            auto id  = pack::ResourceId::parse(params.gunId);
            auto gun = id ? TaczMod::getInstance().gunPacks().gun(*id) : nullptr;
            if (!gun) {
                player->sendMessage("[TACZ] Unknown gun id: " + params.gunId);
                return output.error("Unknown gun id");
            }
            auto state = nbt::freshGun(gun->id, gun->data);
            if (!nbt::writeSelectedGun(*player, state)) return output.error("Hold an item first");
            player->sendMessage(
                "[TACZ] Selected item is now " + gun->id.toString() + " (" + nbt::fireModeName(state.fireMode) + ", "
                + std::to_string(state.ammo) + "+1 rounds)"
            );
        });
}

// Client -> server ping used by the probe HUD (R key). Replies with what the server sees.
class ActionHandler final : public ll::network::PacketHandlerBase<ActionHandler, net::DebugActionPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::DebugActionPacket const& packet) const {
        auto handler = ll::service::getServerNetworkHandler();
        if (!handler) return;
        auto* player = handler->_getServerPlayer(netId, packet.mSenderSubId);
        if (!player) return;

        auto        gun  = nbt::readGun(player->getSelectedItem());
        std::string info = packet.action + " #" + std::to_string(packet.seq) + " from " + player->getRealName() + " gun="
                         + (gun ? gun->gunId.toString() + " " + nbt::fireModeName(gun->fireMode) + " ammo="
                                      + std::to_string(gun->ammo) + (gun->barrel ? "+1" : "")
                                : std::string{"-"});
        logger().info("[probe] {}", info);
        net::DebugAckPacket{packet.seq, std::move(info)}.sendTo(*player);
    }
};

ll::event::ListenerPtr commandListener;

} // namespace

void enable() {
    // Single-player worlds create a fresh server command registry on each world load.
    commandListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerCommandRegisterEvent>(
        [](ll::event::ServerCommandRegisterEvent&) { registerCommands(); }
    );
    // On BDS the registry may already exist by the time the mod is enabled.
    if (ll::service::getCommandRegistry(false)) registerCommands();
}

void disable() {
    if (commandListener) ll::event::EventBus::getInstance().removeListener(commandListener);
    commandListener = nullptr;
}

} // namespace tacz::server::probe
