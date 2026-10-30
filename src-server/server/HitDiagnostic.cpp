#include "server/HitDiagnostic.h"

#include <mutex>
#include <thread>
#include <sstream>
#include <fmt/format.h>
#include "ll/api/io/Logger.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mod/TaczMod.h"

namespace tacz::server::hitdiag {
namespace {
std::mutex mutex;
debug::hit::Budget budget;
debug::hit::PositionHistory history;
Level const* scope = nullptr; // identity only; cleared on server stop/disable
int64_t owner = 0;
uint64_t tickIndex = 0, armedAt = 0;
std::thread::id enableThread;
std::thread::id authThread, levelThread, shootThread;
bool threadLogged = false;
std::string threadId(std::thread::id id) {
    if (id == std::thread::id{}) return "unknown";
    std::ostringstream out; out << id; return out.str();
}
std::string vec(glm::dvec3 p) { return fmt::format("({:.6f},{:.6f},{:.6f})", p.x, p.y, p.z); }
glm::dvec3 fromNative(Vec3 const& p) { return {p.x, p.y, p.z}; }
std::string box(gun::Box const& b) { return vec(b.min) + ":" + vec(b.max); }
void log(debug::hit::Tag tag, std::string const& line) {
    if (scope && budget.take(tag)) TaczMod::getInstance().getSelf().getLogger().info(
        "[hit-diag-server] line={}/{} shot={} triggerTs={} tick={} dt={} {}", budget.used(), debug::hit::LineLimit,
        tag.shot, tag.timestamp, tickIndex, tickIndex - tag.tick, line);
}
}

void enableThreadEvidence() {
    std::lock_guard lock(mutex);
    enableThread = std::this_thread::get_id();
}
void receivedInput(Player const& player) {
    std::lock_guard lock(mutex);
    if (scope && owner == player.getOrCreateUniqueID().rawID)
        authThread = std::this_thread::get_id();
}
void shootHandler(Player const& player) {
    std::lock_guard lock(mutex);
    if (scope && owner == player.getOrCreateUniqueID().rawID)
        shootThread = std::this_thread::get_id();
}
void arm(Player& player, unsigned shots) {
    std::lock_guard lock(mutex);
    budget.arm(shots); history.clear(); scope = shots ? &player.getLevel() : nullptr;
    owner = player.getOrCreateUniqueID().rawID; armedAt = tickIndex;
    authThread = levelThread = shootThread = {}; threadLogged = false;
}
void clear() {
    std::lock_guard lock(mutex);
    budget.arm(0); history.clear(); scope = nullptr; owner = 0;
}
void tick(Level& level) {
    std::lock_guard lock(mutex);
    ++tickIndex;
    if (scope) levelThread = std::this_thread::get_id();
    if (!scope) return;
    if (scope != &level || tickIndex - armedAt >= debug::hit::TickLimit || !budget.available()) {
        budget.arm(0); scope = nullptr; history.clear(); return;
    }
    // At the existing PRE-subTick bullet checkpoint. Native actor positions are
    // read, never written. Retain at most two 4096-actor maps while armed.
    debug::hit::PositionHistory::Samples positions;
    auto sample = [&](Actor& actor) {
        if (positions.size() < 4096 && actor.hasCategory(ActorCategory::Mob))
            positions.emplace(actor.getOrCreateUniqueID().rawID, fromNative(actor.getPosition()));
    };
    level.forEachPlayer([&](Player& player) { sample(player); return true; });
    for (auto const& entity : level.getEntities()) if (auto actor = entity.tryUnwrap()) sample(*actor);
    history.sample(tickIndex, std::move(positions));
}
debug::hit::Tag trigger(Player& player, int64_t timestamp, uint64_t packetClientTick) {
    std::lock_guard lock(mutex);
    if (scope != &player.getLevel() || owner != player.getOrCreateUniqueID().rawID) return {};
    auto tag = budget.trigger(tickIndex, timestamp).value_or(debug::hit::Tag{});
    tag.packetClientTick = packetClientTick;
    return tag;
}
void teleportEvent(Level const* level, int64_t player, gun::TeleportEvent const& event) {
    std::lock_guard lock(mutex);
    if (!scope || scope != level || owner != player || tickIndex - armedAt >= debug::hit::TickLimit) return;
    auto tag = budget.eventTag(tickIndex);
    if (!tag) return;
    auto phase = event.phase == gun::TeleportPhase::Mark ? "mark" : event.phase == gun::TeleportPhase::Ack ? "ack" : "timeout";
    log(*tag, fmt::format("phase=teleport event={} serverTick={} target={} teleportWaitTicks={} ignoredPackets={}",
        phase, event.tick, vec(event.target), event.waitTicks, event.ignored));
}
void spawn(Player& shooter, debug::hit::Tag tag, int serial, int pellet, glm::dvec3 origin,
    glm::dvec3 direction, glm::dvec3 velocity, gun::AuthInputRing const& input,
    glm::dvec3 inherited, gun::RoundInaccuracy const& inaccuracy) {
    if (!tag.generation) return;
    std::lock_guard lock(mutex);
    if (!scope || !budget.accepts(tag)) return;
    if (!threadLogged) {
        // One budgeted line per arming. Unknown means shoot before an armed
        // auth/tick observation; rearm and wait two ticks before the control shot.
        log(tag, fmt::format("phase=threads enableThread={} authThread={} levelTickThread={} shootHandlerThread={} authEqualsLevel={} shootEqualsLevel={} enableEqualsLevel={}",
            threadId(enableThread), threadId(authThread), threadId(levelThread), threadId(shootThread),
            authThread == std::thread::id{} || levelThread == std::thread::id{} ? "unknown" : authThread == levelThread ? "true" : "false",
            shootThread == std::thread::id{} || levelThread == std::thread::id{} ? "unknown" : shootThread == levelThread ? "true" : "false",
            enableThread == std::thread::id{} || levelThread == std::thread::id{} ? "unknown" : enableThread == levelThread ? "true" : "false"));
        threadLogged = true;
    }
    auto current = fromNative(shooter.getPosition());
    auto previous = history.previousPosition(shooter.getOrCreateUniqueID().rawID, tickIndex);
    auto nativePrevious = fromNative(*shooter.mBuiltInComponents->mStateVectorComponent->mPosPrev);
    auto const& rotation = shooter.getRotation();
    auto feet = fromNative(shooter.getFeetPos());
    auto nativeMovement = fromNative(shooter.getPosDelta());
    double eyeHeight = gun::javaEyeHeight(shooter.isSneaking(), shooter.isSwimming(), shooter.isGliding(), shooter.isCrawling());
    log(tag, fmt::format("phase=spawn bullet={} pellet={} shooter={} pos={} previousKnown={} previous={} nativePrevious={} origin={} javaOriginEstimate={} rotation=({:.6f},{:.6f}) direction={} posDelta={} velocity={} packetClientTick={} authKnown={} authClientTick={} authReceiptTick={} authPos={} authRot={} authHeadYaw={} authPreviousPos={} feetPos={} javaEyeHeight={} authFeet={} authPreviousClientTick={} authPreviousReceiptTick={} inherited={} inaccuracyMovement={} inaccuracySpeed={:.6f} inaccuracy={:.6f} inaccuracySource=auth-input-difference inaccuracyType={} inaccuracySamples={} nativeInaccuracySpeed={:.6f} teleportPending={} teleportWaitTicks={} originSource={} authRecordSamples={}",
        serial, pellet, shooter.getOrCreateUniqueID().rawID, vec(current), previous.has_value(),
        previous ? vec(*previous) : "unknown", vec(nativePrevious), vec(origin),
        vec(origin), rotation.x, rotation.z,
        vec(direction), vec(nativeMovement), vec(velocity), tag.packetClientTick, input.latest.has_value(),
        input.latest ? std::to_string(input.latest->clientTick) : "unknown",
        input.latest ? std::to_string(input.latest->receiptTick) : "unknown",
        input.latest ? vec(input.latest->pos) : "unknown",
        input.latest ? fmt::format("({:.6f},{:.6f})", input.latest->rot.x, input.latest->rot.y) : "unknown",
        input.latest ? fmt::format("{:.6f}", input.latest->headYaw) : "unknown",
        input.previous ? vec(input.previous->pos) : "unknown", vec(feet), eyeHeight,
        input.latest ? vec(gun::authInputFeet(input.latest->pos)) : "unknown",
        input.previous ? std::to_string(input.previous->clientTick) : "unknown",
        input.previous ? std::to_string(input.previous->receiptTick) : "unknown",
        vec(inherited), vec(inaccuracy.movement), glm::length(inaccuracy.movement), inaccuracy.value,
        gun::inaccuracyTypeLabel(inaccuracy.type), inaccuracy.samples, glm::length(nativeMovement),
        input.awaitingTeleportAck, input.teleportWaitTicks(shooter.getLevel().getCurrentTick().tickID),
        input.awaitingTeleportAck || !input.latest ? "server-fallback" : "auth-input",
        unsigned(input.latest.has_value()) + unsigned(input.previous.has_value())));
}
void segment(debug::hit::Tag tag, int serial, int age, glm::dvec3 from, glm::dvec3 end,
    glm::dvec3 uncutEnd, bool blockHit) {
    if (!tag.generation) return;
    std::lock_guard lock(mutex);
    if (!scope || !budget.accepts(tag)) return;
    log(tag, fmt::format("phase=segment bullet={} age={} from={} end={} uncutEnd={} blockHit={}",
        serial, age, vec(from), vec(end), vec(uncutEnd), blockHit));
}
void candidate(debug::hit::Tag tag, int serial, int age, Actor& actor, gun::HitboxTarget const& target,
    glm::dvec3 from, glm::dvec3 end) {
    if (!tag.generation) return;
    std::lock_guard lock(mutex);
    if (!scope || !budget.accepts(tag)) return;
    auto gameplay = gun::fixedHitbox(target);
    auto stored = target;
    stored.velocity = fromNative(actor.getPosDelta());
    auto fixed = gun::fixedHitbox(stored);
    auto current = fromNative(actor.getPosition());
    auto previous = history.previousPosition(actor.getOrCreateUniqueID().rawID, tickIndex);
    auto measured = target;
    if (previous) measured.velocity = current - *previous;
    auto measuredFixed = gun::fixedHitbox(measured);
    log(tag, fmt::format("phase=candidate bullet={} age={} target={} runtime={} type={} pos={} previousKnown={} previous={} nativePrevious={} raw={} posDelta={} measured={} fixedDelta={} fixedMeasured={} distanceRaw={:.6f} distanceDelta={:.6f} distanceMeasured={} hitRaw={} hitDelta={} hitMeasured={} player={} crouching={} riding={} gameplayVelocity={} fixedGameplay={} distanceGameplay={:.6f} hitGameplay={}",
        serial, age, actor.getOrCreateUniqueID().rawID, actor.getRuntimeID().rawID, actor.getTypeName(), vec(current),
        previous.has_value(), previous ? vec(*previous) : "unknown", vec(fromNative(*actor.mBuiltInComponents->mStateVectorComponent->mPosPrev)),
        box(target.box), vec(stored.velocity), previous ? vec(measured.velocity) : "unknown", box(fixed),
        previous ? box(measuredFixed) : "unknown", debug::hit::segmentBoxDistance(target.box, from, end),
        debug::hit::segmentBoxDistance(fixed, from, end),
        previous ? fmt::format("{:.6f}", debug::hit::segmentBoxDistance(measuredFixed, from, end)) : "unknown",
        gun::clip(target.box, from, end).has_value(), gun::clip(fixed, from, end).has_value(),
        previous ? (gun::clip(measuredFixed, from, end) ? "true" : "false") : "unknown",
        target.isPlayer, target.isCrouching, target.isRiding, vec(target.velocity), box(gameplay),
        debug::hit::segmentBoxDistance(gameplay, from, end), gun::clip(gameplay, from, end).has_value()));
}
} // namespace tacz::server::hitdiag
