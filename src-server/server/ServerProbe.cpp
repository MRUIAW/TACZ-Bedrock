#include "server/ServerProbe.h"

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
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/nbt/IntTag.h"
#include "mc/deps/nbt/StringTag.h"
#include "mc/network/ServerNetworkHandler.h"
#include "mc/server/ServerPlayer.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/ItemStack.h"

#include "debug/RenderDebug.h"
#include "mod/TaczMod.h"
#include "nbt/GunNbt.h"
#include "net/DebugPackets.h"
#include "server/BulletSystem.h"

namespace tacz::server::probe {
namespace {

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

// Reads GunId / ammo from an item's user data; empty id if it is not a gun.
struct GunState {
    std::string id;
    int         ammo = 0;
};

GunState readGun(ItemStack const& item) {
    GunState state;
    auto const& tag = item.mUserData;
    if (!tag || !tag->contains(nbt::GunId, Tag::String)) return state;
    state.id = static_cast<std::string const&>((*tag)[nbt::GunId]);
    if (tag->contains(nbt::GunCurrentAmmoCount, Tag::Int)) {
        state.ammo = static_cast<int>((*tag)[nbt::GunCurrentAmmoCount]);
    }
    return state;
}

// Writes GunId / ammo into the player's selected item and syncs it to the client.
bool writeGun(Player& player, std::string const& gunId, int ammo) {
    ItemStack item(player.getSelectedItem());
    if (item.isNull()) return false;
    auto tag = item.mUserData ? item.mUserData->clone() : std::make_unique<CompoundTag>();
    (*tag)[nbt::GunId]               = StringTag{gunId};
    (*tag)[nbt::GunCurrentAmmoCount] = IntTag{ammo};
    item.setUserData(std::move(tag));
    player.setSelectedItem(item);
    player.sendInventory(false);
    return true;
}

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
    // /tacz setgun <gunId> : mark the selected item as a gun with a full magazine
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
            int ammo = gun->data.ammoAmount;
            if (!writeGun(*player, gun->id.toString(), ammo)) return output.error("Hold an item first");
            player->sendMessage("[TACZ] Selected item is now " + gun->id.toString() + " with " + std::to_string(ammo) + " rounds");
        });
}

// Client -> server action handler. Replies with what the server sees, and
// consumes one round on "fire" to prove the NBT round trip.
class ActionHandler final : public ll::network::PacketHandlerBase<ActionHandler, net::DebugActionPacket> {
public:
    void handlePacket(NetworkIdentifier const& netId, NetEventCallback&, net::DebugActionPacket const& packet) const {
        auto handler = ll::service::getServerNetworkHandler();
        if (!handler) return;
        auto* player = handler->_getServerPlayer(netId, packet.mSenderSubId);
        if (!player) return;

        auto gun = readGun(player->getSelectedItem());
        if (packet.action == "fire" && !gun.id.empty() && gun.ammo > 0) {
            gun.ammo--;
            writeGun(*player, gun.id, gun.ammo);
            auto id    = pack::ResourceId::parse(gun.id);
            auto entry = id ? TaczMod::getInstance().gunPacks().gun(*id) : nullptr;
            // Fire mode switching is not implemented yet: use the gun's first mode (validated non-empty).
            if (entry) bullet::fire(*player, entry->data, *entry->data.fireModes.front());
        }
        std::string info = packet.action + " #" + std::to_string(packet.seq) + " from " + player->getRealName()
                         + " gun=" + (gun.id.empty() ? std::string{"-"} : gun.id) + " ammo=" + std::to_string(gun.ammo);
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
