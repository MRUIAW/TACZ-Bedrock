#include "server/ServerProbe.h"

#include <algorithm>
#include <memory>
#include <string>
#include <set>

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
#include "debug/AmmoSyncDebug.h"
#include "gun/ShootRules.h"
#include "mod/TaczMod.h"
#include "nbt/AmmoItem.h"
#include "nbt/GunItem.h"
#include "nbt/PackItem.h"
#include "net/DebugPackets.h"
#include "server/HitDiagnostic.h"
#include "server/BulletSystem.h"

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
struct GunListParams { std::string namespaceName; };

struct RenderDebugParams {
    int material;
    int twoSided;
};
struct RenderToggleParams { int value; };

struct BulletDebugParams {
    int   moveMode;
    float speedScale;
};
struct HitDiagnosticParams { int shots; };

struct AmmoParams {
    int count;
};
struct GiveAmmoParams { std::string ammoId; int count=1; };

struct AnimationParams {
    std::string name;
};

namespace {

void registerCommands() {
    auto& command = ll::command::CommandRegistrar::getServerInstance().getOrCreateCommand(
        "tacz",
        "TACZ items and diagnostics",
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
        for (auto const& source:packs.packs()) {
            size_t count=0,missing=0;
            std::set<std::string> namespaces;
            for (auto const& [id,entry]:packs.guns()) if (entry.packName==source.name) {
                ++count; namespaces.insert(id.ns); missing+=packs.assetIssues().contains(id);
            }
            std::string ns;
            for (auto const& name:namespaces) ns+=(ns.empty() ? "" : ",")+name;
            auto line="[TACZ pack] "+source.name+" namespace="+(ns.empty() ? source.meta.ns : ns)
                +" guns="+std::to_string(count)+" missingAssets="+std::to_string(missing);
            logger().info("{}",line);
            if (auto* player=originPlayer(origin)) player->sendMessage(line);
            else output.success(line);
        }
    });
    auto listGuns=[](CommandOrigin const& origin,CommandOutput& output,std::string const& ns) {
        auto const& packs=TaczMod::getInstance().gunPacks();
        std::vector<std::string> chunks;
        std::string chunk;
        size_t count=0;
        for (auto const& [id,entry]:packs.guns()) if (ns.empty() || id.ns==ns) {
            auto name=id.toString();
            if (!chunk.empty() && chunk.size()+name.size()+2>180) { chunks.push_back(std::move(chunk)); chunk.clear(); }
            chunk+=(chunk.empty() ? "" : ", ")+name;
            ++count;
        }
        if (!chunk.empty()) chunks.push_back(std::move(chunk));
        auto send=[&](std::string const& line) {
            logger().info("{}",line);
            if (auto* player=originPlayer(origin)) player->sendMessage(line); else output.success(line);
        };
        send("[TACZ guns] namespace="+(ns.empty() ? std::string("all") : ns)+" count="+std::to_string(count));
        for (size_t i=0;i<chunks.size();++i) send("["+std::to_string(i+1)+"/"+std::to_string(chunks.size())+"] "+chunks[i]);
    };
    command.overload().text("guns").execute([listGuns](CommandOrigin const& origin,CommandOutput& output) { listGuns(origin,output,{}); });
    command.overload<GunListParams>().text("guns").required("namespaceName")
        .execute([listGuns](CommandOrigin const& origin,CommandOutput& output,GunListParams const& params) { listGuns(origin,output,params.namespaceName); });
    command.overload<AnimationParams>()
        .text("anim")
        .required("name")
        .execute([](CommandOrigin const& origin, CommandOutput& output, AnimationParams const& params) {
            auto* player = originPlayer(origin);
            if (!player) return output.error("Player only");
            auto gun = nbt::readGun(player->getSelectedItem());
            if (!gun) return output.error("Hold a gun first");
            net::AnimationDebugPacket{gun->gunId.toString(), params.name}.sendTo(*player);
            player->sendMessage("[TACZ] animation: " + params.name + " (see client log for available names)");
            output.success("Animation request sent");
        });
    // /tacz rdebug <material> <twoSided> : first-person gun render diagnostics
    command.overload().text("rdebug").text("hud").execute([](CommandOrigin const&,CommandOutput& output) {
        ++debug::hudRevision;
        output.success("HUD metrics/ammo snapshot requested; see client [hud-layout] log (integrated client only)");
    });
    command.overload<RenderToggleParams>().text("rdebug").text("sampler").required("value")
        .execute([](CommandOrigin const&,CommandOutput& output,RenderToggleParams const& params) {
            if (params.value<0 || params.value>1) return output.error("sampler: 0 legacy upload, 1 explicit single mip/no dropping (not a point/linear switch)");
            debug::gunSampler=params.value; ++debug::renderRevision;
            output.success("Gun texture upload changed; no shared sampler state modified (integrated client only)");
        });
    command.overload<RenderToggleParams>().text("rdebug").text("blend").required("value")
        .execute([](CommandOrigin const&,CommandOutput& output,RenderToggleParams const& params) {
            if (params.value<0 || params.value>1) return output.error("blend: 0 legacy, 1 TACZ display transparency/cutout (default)");
            debug::gunBlend=params.value!=0; ++debug::renderRevision;
            output.success("Gun alpha compatibility changed (integrated client only)");
        });
    command.overload<RenderToggleParams>().text("rdebug").text("probe").required("value")
        .execute([](CommandOrigin const&,CommandOutput& output,RenderToggleParams const& params) {
            if (params.value<0 || params.value>1) return output.error("probe: 0 or 1 (default off)");
            debug::probeHud=params.value!=0;
            output.success("Probe HUD changed (integrated client only)");
        });
    command.overload<RenderToggleParams>().text("rdebug").text("path").required("value")
        .execute([](CommandOrigin const&, CommandOutput& output, RenderToggleParams const& params) {
            if (params.value<0 || params.value>1) return output.error("path: 0 baked, 1 rigid bone matrices (default)");
            debug::gunRenderPath=params.value;
            ++debug::renderRevision;
            output.success("Gun render path changed (integrated client only)");
        });
    command.overload<RenderToggleParams>().text("rdebug").text("mesh").required("value")
        .execute([](CommandOrigin const&, CommandOutput& output, RenderToggleParams const& params) {
            if (params.value < 0 || params.value > 1) return output.error("mesh: 0 or 1");
            debug::meshDiagnostics = params.value != 0;
            ++debug::renderRevision;
            output.success("Mesh diagnostics changed (integrated client only)");
        });
    command.overload<RenderToggleParams>().text("rdebug").text("freeze").required("value")
        .execute([](CommandOrigin const& origin, CommandOutput& output, RenderToggleParams const& params) {
            if (params.value < 0 || params.value > 1) return output.error("freeze: 0 or 1");
            debug::freezePose = params.value != 0;
            ++debug::renderRevision;
            if (auto* player = originPlayer(origin)) player->sendMessage("[TACZ] freeze=" + std::to_string(params.value) + " (integrated client only)");
            output.success("Render pose freeze changed");
        });
    command.overload<RenderToggleParams>().text("rdebug").text("ammosync").required("value")
        .execute([](CommandOrigin const&,CommandOutput& output,RenderToggleParams const& params) {
            if (params.value<0 || params.value>1) return output.error("ammosync: 0 or 1 (default off; 1 re-arms capped logs)");
            debug::setAmmoSync(params.value!=0);
            output.success("Ammo sync diagnostics changed (256 samples/path; integrated client shares switch)");
        });
    command.overload<RenderToggleParams>().text("rdebug").text("depth").required("value")
        .execute([](CommandOrigin const& origin, CommandOutput& output, RenderToggleParams const& params) {
            if (params.value < 0 || params.value > 2) return output.error("depth: 0 native, 1 near=.05 far=16, 2 near=.10 far=16");
            debug::gunDepthMode = params.value;
            ++debug::renderRevision;
            if (auto* player = originPlayer(origin)) player->sendMessage("[TACZ] depth=" + std::to_string(params.value) + " (integrated client only; no world depth clear)");
            output.success("Render depth diagnostic changed");
        });
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
    // Read-only hit diagnostics; independent server/client budgets.
    for (auto side : {std::string("server"), std::string("client")}) {
        command.overload<HitDiagnosticParams>().text("hitdiag").text(side).required("shots")
            .execute([side](CommandOrigin const& origin, CommandOutput& output, HitDiagnosticParams const& params) {
                auto* player = originPlayer(origin);
                if (!player) return output.error("Player only (diagnostic is scoped to the issuing admin)");
                if (params.shots < 0 || params.shots > static_cast<int>(debug::hit::ShotLimit))
                    return output.error("shots: 0 stops; 1..16 arms next accepted triggers, 64 lines, 400 ticks");
                if (side == "server") hitdiag::arm(*player, params.shots);
                else net::HitDiagnosticPacket{static_cast<unsigned>(params.shots)}.sendTo(*player);
                auto message = "[TACZ] hitdiag " + side + " shots=" + std::to_string(params.shots) + " (64 lines; read-only)";
                player->sendMessage(message); output.success(message);
            });
    }
    command.overload().text("bullet").text("status")
        .execute([](CommandOrigin const& origin, CommandOutput& output) {
            auto status = bullet::experimentStatus();
            if (auto* player = originPlayer(origin)) player->sendMessage("[TACZ] " + status);
            output.success(status);
        });
    command.overload<HitDiagnosticParams>().text("bullet").text("diag").required("shots")
        .execute([](CommandOrigin const& origin, CommandOutput& output, HitDiagnosticParams const& params) {
            auto* player = originPlayer(origin); if (!player) return output.error("Player only");
            if (params.shots < 0 || params.shots > 16) return output.error("0 stops; 1..16 next pellets; 400 ticks/bullet");
            bullet::armDiagnostic(*player, params.shots);
            player->sendMessage("[TACZ] bullet server diagnostic armed: " + std::to_string(params.shots));
            output.success("Bullet server diagnostic armed: " + std::to_string(params.shots));
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
    auto giveAmmo=[](Player& player,pack::ResourceId const& id,int count) {
        int left=std::clamp(count,1,64*36),given=0;
        given=nbt::giveAmmo(player,id,left); left-=given;
        player.sendMessage("[TACZ] Gave "+std::to_string(given)+" x "+id.toString()+(left ? " (item registration or overflow drop failed)" : ""));
    };
    command.overload<GiveAmmoParams>().text("give").text("ammo").required("ammoId").optional("count")
        .execute([giveAmmo](CommandOrigin const& origin,CommandOutput& output,GiveAmmoParams const& params) {
            auto* player=originPlayer(origin); if (!player) return output.error("Player only");
            auto id=pack::ResourceId::parse(params.ammoId);
            if (!id || !TaczMod::getInstance().gunPacks().ammo(*id)) return output.error("Unknown ammo id");
            if (params.count<1 || params.count>64*36) return output.error("count: 1..2304");
            giveAmmo(*player,*id,params.count);
        });
    // /tacz ammo <count> : real TACZ ammo for the held gun, including legacy guns.
    command.overload<AmmoParams>()
        .text("ammo")
        .required("count")
        .execute([giveAmmo](CommandOrigin const& origin, CommandOutput& output, AmmoParams const& params) {
            auto* player = originPlayer(origin);
            if (!player) return output.error("Player only");
            auto gun   = nbt::readGun(player->getSelectedItem());
            auto entry = gun ? TaczMod::getInstance().gunPacks().gun(gun->gunId) : nullptr;
            if (!entry || !entry->data.ammoId) {
                player->sendMessage("[TACZ] Hold a gun first");
                return output.error("Hold a gun first");
            }
            if (params.count<1 || params.count>64*36) return output.error("count: 1..2304");
            giveAmmo(*player,*entry->data.ammoId,params.count);
        });
    auto giveGun=[](CommandOrigin const& origin, CommandOutput& output, SetGunParams const& params) {
            auto* player = originPlayer(origin);
            if (!player) return output.error("Player only");
            auto id  = pack::ResourceId::parse(params.gunId);
            auto gun = id ? TaczMod::getInstance().gunPacks().gun(*id) : nullptr;
            if (!gun) {
                player->sendMessage("[TACZ] Unknown gun id: " + params.gunId);
                return output.error("Unknown gun id");
            }
            if (!nbt::giveGun(*player,gun->id)) return output.error("Inventory full or tacz item BP unavailable");
            player->sendMessage("[TACZ] Gave "+gun->id.toString()+" (creative-tab initial NBT)");
    };
    command.overload<SetGunParams>().text("give").text("gun").required("gunId").execute(giveGun);
    // Compatibility command alias; old vanilla stacks with GunId still work.
    command.overload<SetGunParams>().text("setgun").required("gunId").execute(giveGun);
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
        auto const* entry=gun ? TaczMod::getInstance().gunPacks().gun(gun->gunId) : nullptr;
        bool chambered=entry && gun::hasChamberedRound(entry->data.bolt,gun->barrel);
        std::string info = packet.action + " #" + std::to_string(packet.seq) + " from " + player->getRealName() + " gun="
                          + (gun ? gun->gunId.toString() + " " + nbt::fireModeName(gun->fireMode) + " ammo="
                                       + std::to_string(gun->ammo) + (chambered ? "+1" : "")
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
