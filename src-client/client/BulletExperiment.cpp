#include "client/BulletExperiment.h"

#include <atomic>
#include <mutex>
#include <type_traits>
#include <vector>
#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ClientCommandRegisterEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/network/packet/MoveActorAbsolutePacket.h"
#include "mc/network/packet/MoveActorDeltaPacket.h"
#include "mc/network/packet/SetActorMotionPacket.h"
#include "mc/network/packet/MotionPredictionHintsPacket.h"
#include "mc/world/actor/ActorDataIDs.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/entity/components/MovementInterpolatorComponent.h"
#include "client/gameplay/ClientShooter.h"
#include "gun/BulletExperiment.h"
#include "mod/TaczMod.h"
#include "native/BulletWater.h"
#include "net/BulletPacket.h"

namespace tacz::client::bullet {
// External linkage for LL command reflection.
struct ModeParams { std::string mode; };
struct CountParams { int count; };
namespace {
using Mode = gun::bullet::DriveMode;
std::mutex mutex;
gun::bullet::States states;
struct Envelope { uint64_t world; gun::bullet::Message message; };
std::vector<Envelope> inbox;
struct PlacementEnvelope { uint64_t world; gun::bullet::Placement placement; };
std::vector<PlacementEnvelope> placementInbox;
gun::bullet::DiagnosticBudget budget;
std::atomic<Mode> driveMode{Mode::Off};
std::atomic<bool> marker{true};
std::atomic<bool> hooksReady{false};
uint64_t scopeWorld = 0, ticks = 0;
std::optional<int> dimension;
ll::event::ListenerPtr commandListener;
auto& logger() { return TaczMod::getInstance().getSelf().getLogger(); }
glm::dvec3 fromNative(Vec3 const& p) { return {p.x,p.y,p.z}; }
Vec3 toNative(glm::dvec3 p) { return {static_cast<float>(p.x),static_cast<float>(p.y),static_cast<float>(p.z)}; }
char const* modeName(Mode mode) { return mode == Mode::Off ? "off" : mode == Mode::Drive ? "drive" : "drive-filter"; }
void summary(gun::bullet::State& b, char const* reason) {
    if (!b.diagnostic) return;
    logger().info("[bullet-exp-client-summary] serial={} ticks={} overwrites={} unexplained={} received={} filtered={} absolute={} delta={} motion={} hints={} maxSnap={:.6f} unaged={} reason={}",
        b.spawn.serial,b.ticks,b.overwrites,b.unexplained,b.received,b.filtered,b.absolute,b.delta,b.nativeMotion,b.hints,b.maxSnap,b.unaged,reason);
    b.diagnostic = false;
}
void clearLocked(char const* reason) {
    for (auto& [id,b] : states.entries) summary(b,reason);
    states.clear(); budget.arm(0,ticks); scopeWorld = 0; dimension.reset();
}
class Handler final : public ll::network::PacketHandlerBase<Handler,net::BulletPacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, net::BulletPacket const& packet) const {
        auto world = shooter::worldToken(); if (!world || !packet.message) return;
        std::lock_guard lock(mutex);
        inbox.push_back({world,*packet.message}); // no native/world service access from packet thread
    }
};
bool addPlacement(gun::bullet::Placement const& m, uint64_t world) {
    if (scopeWorld != world || !dimension || *dimension != m.dimension) return false;
    auto it = states.entries.find(m.serial);
    if (it == states.entries.end() || it->second.spawn.runtime != m.runtime || it->second.spawn.unique != m.unique) return false;
    it->second.placement(m.age,m.pos); return true;
}
class PlacementHandler final : public ll::network::PacketHandlerBase<PlacementHandler,net::BulletPlacementPacket> {
public:
    void handlePacket(NetworkIdentifier const&,NetEventCallback&,net::BulletPlacementPacket const& packet) const {
        auto world = shooter::worldToken(); if (!world || !packet.placement) return;
        std::lock_guard lock(mutex);
        // Pure metadata can be installed before the following native receive hook,
        // without looking up any native actor or service on the packet thread.
        if (!addPlacement(*packet.placement,world)) placementInbox.push_back({world,*packet.placement});
    }
};
Actor* resolve(ClientLevel& level, gun::bullet::State const& b) {
    auto* actor = level.getRuntimeEntity(ActorRuntimeID{b.spawn.runtime},false);
    return actor && actor->getOrCreateUniqueID().rawID == b.spawn.unique ? actor : nullptr;
}
// Called by all four supported, typed receive hooks. Unknown actors pass through untouched.
// Delta baselines consume EVERY received field even when native application is filtered.
bool movement(uint64_t runtime, int kind, glm::dvec3 position, std::array<bool,3> axes) {
    if (!shooter::worldToken()) return false;
    for (int i = 0; i < 3; ++i) if (axes[i] && !std::isfinite(position[i])) return false;
    std::lock_guard lock(mutex);
    if (scopeWorld != shooter::worldToken()) return false;
    auto* b = states.runtime(runtime); if (!b) return false;
    auto receipt = b->receive(driveMode.load(),kind,position,axes);
    return receipt.filtered;
}
using AbsoluteSignature = void (LegacyClientNetworkHandler::*)(NetworkIdentifier const&,MoveActorAbsolutePacket const&);
using DeltaSignature = void (LegacyClientNetworkHandler::*)(NetworkIdentifier const&,MoveActorDeltaPacket const&);
using MotionSignature = void (LegacyClientNetworkHandler::*)(NetworkIdentifier const&,SetActorMotionPacket const&);
using HintsSignature = void (LegacyClientNetworkHandler::*)(NetworkIdentifier const&,MotionPredictionHintsPacket const&);
static_assert(std::is_same_v<decltype(static_cast<AbsoluteSignature>(&LegacyClientNetworkHandler::$handle)),AbsoluteSignature>);
static_assert(std::is_same_v<decltype(static_cast<DeltaSignature>(&LegacyClientNetworkHandler::$handle)),DeltaSignature>);
static_assert(std::is_same_v<decltype(static_cast<MotionSignature>(&LegacyClientNetworkHandler::$handle)),MotionSignature>);
static_assert(std::is_same_v<decltype(static_cast<HintsSignature>(&LegacyClientNetworkHandler::$handle)),HintsSignature>);
static_assert(std::is_same_v<decltype(&Actor::_setPos),void (Actor::*)(Vec3 const&)>);
LL_TYPE_INSTANCE_HOOK(BulletAbsolute,ll::memory::HookPriority::Normal,LegacyClientNetworkHandler,
    static_cast<AbsoluteSignature>(&LegacyClientNetworkHandler::$handle),void,NetworkIdentifier const& source,MoveActorAbsolutePacket const& packet) {
    auto const& data = *packet.mMoveData;
    if (!movement(data.mRuntimeId->rawID,0,fromNative(*data.mPos),{true,true,true})) origin(source,packet);
}
LL_TYPE_INSTANCE_HOOK(BulletDelta,ll::memory::HookPriority::Normal,LegacyClientNetworkHandler,
    static_cast<DeltaSignature>(&LegacyClientNetworkHandler::$handle),void,NetworkIdentifier const& source,MoveActorDeltaPacket const& packet) {
    auto const& data = *packet.mMoveData;
    std::array<bool,3> axes{data.mHeader->mContainsPositionX,data.mHeader->mContainsPositionY,data.mHeader->mContainsPositionZ};
    glm::dvec3 position{0}; // absent fields need not have been initialized by the native decoder
    if (axes[0]) position.x = data.mNewPositionX;
    if (axes[1]) position.y = data.mNewPositionY;
    if (axes[2]) position.z = data.mNewPositionZ;
    if (!movement(data.mRuntimeId->rawID,1,position,axes)) origin(source,packet);
}
LL_TYPE_INSTANCE_HOOK(BulletMotion,ll::memory::HookPriority::Normal,LegacyClientNetworkHandler,
    static_cast<MotionSignature>(&LegacyClientNetworkHandler::$handle),void,NetworkIdentifier const& source,SetActorMotionPacket const& packet) {
    if (!movement(packet.mRuntimeId->rawID,2,{},{})) origin(source,packet);
}
LL_TYPE_INSTANCE_HOOK(BulletHints,ll::memory::HookPriority::Normal,LegacyClientNetworkHandler,
    static_cast<HintsSignature>(&LegacyClientNetworkHandler::$handle),void,NetworkIdentifier const& source,MotionPredictionHintsPacket const& packet) {
    if (!movement(packet.mRuntimeId->rawID,3,{},{})) origin(source,packet);
}
bool hookAttempted = false;
std::array<bool,4> installed{};
bool activeCommand(CommandOutput& output) {
    if (shooter::worldToken()) return true;
    output.error("No world session"); return false;
}
void feedback(CommandOutput& output, std::string const& message) {
    output.success(message); logger().info("[bullet-exp-control-client] {}",message);
    if (!shooter::worldToken()) return;
    auto client = ll::service::getClientInstance();
    if (client && shooter::ownsClient(&*client)) if (auto* player = client->getLocalPlayer())
        player->displayClientMessage("[TACZ] " + message,std::nullopt);
}
void registerCommands() {
    auto& command = ll::command::CommandRegistrar::getClientInstance().getOrCreateCommand("taczbullet","TACZ bullet experiment (client)");
    command.overload<ModeParams>().text("mode").required("mode")
        .execute([](CommandOrigin const&,CommandOutput& output,ModeParams const& params) {
            if (!activeCommand(output)) return;
            if (params.mode != "off" && params.mode != "drive" && params.mode != "drive-filter")
                return output.error("mode: off, drive, drive-filter");
            if (params.mode != "off" && !(installed[0] && installed[1]))
                return output.error("Position correction hooks incomplete; driving unavailable (see bullet-exp-hooks log)");
            if (params.mode == "drive-filter" && !hooksReady.load())
                return output.error("Movement receive hooks incomplete; drive-filter unavailable (see bullet-exp-hooks log)");
            std::lock_guard lock(mutex);
            driveMode = params.mode == "off" ? Mode::Off : params.mode == "drive" ? Mode::Drive : Mode::Filter;
            for (auto& [id,b] : states.entries) b.wrote = false;
            feedback(output,"Client bullet mode: " + params.mode);
        });
    command.overload<CountParams>().text("marker").required("count")
        .execute([](CommandOrigin const&,CommandOutput& output,CountParams const& params) {
            if (!activeCommand(output)) return;
            if (params.count < 0 || params.count > 1) return output.error("marker: 0 or 1 (default 1)");
            marker = params.count != 0; feedback(output,"Client bullet marker: " + std::to_string(params.count));
        });
    command.overload<CountParams>().text("diag").required("count")
        .execute([](CommandOrigin const&,CommandOutput& output,CountParams const& params) {
            if (!activeCommand(output)) return;
            if (params.count < 0 || params.count > 16) return output.error("diag: 0 stops; 1..16 next pellets; 400 ticks/bullet");
            std::lock_guard lock(mutex);
            for (auto& [id,b] : states.entries) summary(b,"rearm");
            budget.arm(params.count,ticks);
            feedback(output,"Client bullet diagnostic armed: " + std::to_string(params.count));
        });
    command.overload().text("status").execute([](CommandOrigin const&,CommandOutput& output) {
        if (!activeCommand(output)) return;
        std::lock_guard lock(mutex);
        feedback(output,std::string("client mode=") + modeName(driveMode.load()) + " marker="
            + std::to_string(marker.load()) + " active=" + std::to_string(states.entries.size())
            + " hooksReady=" + std::to_string(hooksReady.load()) + " postTicks=" + std::to_string(ticks)
            + " scopedWorld=" + std::to_string(scopeWorld));
    });
}
}
void clear() {
    std::lock_guard lock(mutex); clearLocked("world-exit"); inbox.clear(); placementInbox.clear(); driveMode = Mode::Off;
}
void tick(ClientLevel& level) {
    auto world = shooter::worldToken(); if (!world) return;
    auto client = ll::service::getClientInstance();
    if (!client || !shooter::ownsClient(&*client) || client->getLevel() != &level) return;
    auto* player = client->getLocalPlayer(); if (!player) return;
    int dim = player->getDimensionId();
    std::lock_guard lock(mutex); ++ticks;
    if (scopeWorld != world || (dimension && *dimension != dim)) clearLocked("scope-change");
    scopeWorld = world; dimension = dim;
    for (auto const& envelope : inbox) {
        auto const& m = envelope.message;
        if (envelope.world != world || m.dimension != dim) continue;
        if (m.kind == gun::bullet::Kind::Spawn) {
            if (states.spawn(m)) {
                auto& b = states.entries.at(m.serial); b.diagnostic = budget.take(ticks);
                if (b.diagnostic) logger().info("[bullet-exp-client] serial={} runtime={} age=0 phase=spawn mode={}",m.serial,m.runtime,modeName(driveMode.load()));
            }
        } else if (auto it = states.entries.find(m.serial); it != states.entries.end()
            && it->second.spawn.runtime == m.runtime && it->second.spawn.unique == m.unique) {
            if (auto* actor = resolve(level,it->second)) actor->remove();
            summary(it->second,"server-remove"); states.remove(m.serial);
        }
    }
    inbox.clear();
    for (auto const& envelope : placementInbox) if (envelope.world == world) addPlacement(envelope.placement,world);
    placementInbox.clear(); // unknown/late serials never create state
    for (auto it = states.entries.begin(); it != states.entries.end();) {
        auto& b = it->second;
        auto* actor = resolve(level,b);
        glm::dvec3 before = actor ? fromNative(actor->getPosition()) : glm::dvec3{};
        auto expected = b.lastWritten;
        bool overwritten = actor && b.observeBefore(before);
        auto interpolator = actor ? actor->getEntityContext().tryGetComponent<MovementInterpolatorComponent>()
                                  : optional_ref<MovementInterpolatorComponent>{};
        glm::dvec3 interpBefore = interpolator ? fromNative(*interpolator->mPos) : glm::dvec3{};
        std::array<int,3> stepsBefore{};
        if (interpolator) stepsBefore = {interpolator->mPositionSteps,interpolator->mRotationSteps,interpolator->mHeadYawSteps};
        if (actor && !interpolator && b.diagnostic && !b.interpolatorMissingLogged) {
            logger().info("[bullet-exp-client] serial={} phase=interpolator-missing (continuing without component creation)",b.spawn.serial);
            b.interpolatorMissingLogged = true;
        }
        bool water = false;
        bool alive = b.tick([&](glm::dvec3 position) {
            water = actor && native::bulletInWater(actor->getDimensionBlockSource(),position); return water;
        });
        bool driving = driveMode.load() != Mode::Off;
        glm::dvec3 half{};
        if (actor) {
            int variant = marker.load() ? 0 : 1;
            if (actor->mEntityData->getInt(static_cast<ushort>(ActorDataIDs::Variant)) != variant) {
                std::vector<std::unique_ptr<DataItem>> values;
                values.push_back(DataItem::create(ActorDataIDs::Variant,variant));
                actor->mEntityData->assignValues(values,*actor,std::nullopt);
            }
            if (driving) {
                actor->_setPos(toNative(b.motion.pos)); // refreshes native spatial/AABB state
                actor->mBuiltInComponents->mStateVectorComponent->mPosPrev = toNative(b.previous);
                if (interpolator) {
                    auto rot = b.motion.rotation();
                    gun::bullet::overrideInterpolator(*interpolator,toNative(b.motion.pos),Vec2{rot.x,rot.y});
                }
                b.lastWritten = fromNative(actor->getPosition());
                half = fromNative(actor->getInterpolatedPosition(.5f));
            }
        }
        b.wrote = driving && actor;
        if (b.diagnostic) {
            for (auto const& c : b.applied) if (b.correctionLines < 64) {
                ++b.correctionLines;
                logger().info("[bullet-exp-client-correction] serial={} age={} positionAge={} packet={} phase={} snap={:.6f} target=({:.6f},{:.6f},{:.6f})",
                    b.spawn.serial,b.motion.age,c.correction.age,c.correction.kind == 0 ? "absolute" : "delta",
                    c.afterStep ? "after-step" : "before-step",c.snap,c.correction.pos.x,c.correction.pos.y,c.correction.pos.z);
            }
            auto used = interpolator ? fromNative(*interpolator->mPos) : glm::dvec3{};
            auto phase = b.applied.empty() ? "none" : b.applied.back().afterStep ? "after-step" : "before-step";
            logger().info("[bullet-exp-client] serial={} age={} tick={} mode={} sim=({:.6f},{:.6f},{:.6f}) before=({:.6f},{:.6f},{:.6f}) lastWrite=({:.6f},{:.6f},{:.6f}) previous=({:.6f},{:.6f},{:.6f}) interpHalf=({:.6f},{:.6f},{:.6f}) actorMissing={} wrote={} overwritten={} received={} filtered={} snap={:.6f} water={} interpolatorPresent={} interpTargetBefore=({:.6f},{:.6f},{:.6f}) interpStepsBefore=({},{},{}) interpTargetUsed=({:.6f},{:.6f},{:.6f}) interpStepsUsed=({},{},{}) positionAge={} snapPhase={} unaged={}",
                b.spawn.serial,b.motion.age,ticks,modeName(driveMode.load()),b.motion.pos.x,b.motion.pos.y,b.motion.pos.z,
                before.x,before.y,before.z,expected.x,expected.y,expected.z,b.previous.x,b.previous.y,b.previous.z,half.x,half.y,half.z,
                !actor,b.wrote,overwritten,b.arrivedTick,b.filteredTick,b.tickSnap,water,static_cast<bool>(interpolator),
                interpBefore.x,interpBefore.y,interpBefore.z,stepsBefore[0],stepsBefore[1],stepsBefore[2],used.x,used.y,used.z,
                interpolator ? int(interpolator->mPositionSteps) : -1,interpolator ? int(interpolator->mRotationSteps) : -1,
                interpolator ? int(interpolator->mHeadYawSteps) : -1,b.lastPacketAge,phase,b.unaged);
            if (b.ticks >= 400) summary(b,"diagnostic-window");
        }
        b.arrivedTick = b.filteredTick = b.acceptedTick = 0; b.tickSnap = 0;
        if (!alive) {
            if (actor && driving) actor->remove();
            summary(b,"local-expire"); auto serial = it->first; ++it; states.remove(serial);
        } else ++it;
    }
}
void enable() {
    commandListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientCommandRegisterEvent>(
        [](auto&) { registerCommands(); });
    if (!hookAttempted) {
        installed = {BulletAbsolute::hook() == 0,BulletDelta::hook() == 0,BulletMotion::hook() == 0,BulletHints::hook() == 0};
        hooksReady = installed[0] && installed[1] && installed[2] && installed[3];
        hookAttempted = true;
        logger().info("[bullet-exp-hooks] absolute={} delta={} motion={} hints={} ready={}",
            installed[0],installed[1],installed[2],installed[3],hooksReady.load());
    }
}
void disable() {
    clear();
    if (installed[3]) BulletHints::unhook();
    if (installed[2]) BulletMotion::unhook();
    if (installed[1]) BulletDelta::unhook();
    if (installed[0]) BulletAbsolute::unhook();
    installed = {}; hooksReady = false; hookAttempted = false;
    if (commandListener) ll::event::EventBus::getInstance().removeListener(commandListener);
    commandListener = nullptr;
}
}
