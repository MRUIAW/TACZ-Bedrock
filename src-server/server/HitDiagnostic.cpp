#include "server/HitDiagnostic.h"

#include <mutex>
#include <fmt/format.h>
#include "ll/api/io/Logger.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mod/TaczMod.h"

namespace tacz::server::hitdiag {
namespace {
std::mutex mutex;
debug::hit::Budget budget;
debug::hit::PositionHistory history;
Level const* scope = nullptr; // identity only; cleared on server stop/disable
int64_t owner = 0;
uint64_t tickIndex = 0, armedAt = 0;
std::string vec(glm::dvec3 p) { return fmt::format("({:.6f},{:.6f},{:.6f})", p.x, p.y, p.z); }
glm::dvec3 fromNative(Vec3 const& p) { return {p.x, p.y, p.z}; }
std::string box(gun::Box const& b) { return vec(b.min) + ":" + vec(b.max); }
void log(debug::hit::Tag tag, std::string const& line) {
    if (scope && budget.take(tag)) TaczMod::getInstance().getSelf().getLogger().info(
        "[hit-diag-server] line={}/{} shot={} triggerTs={} tick={} dt={} {}", budget.used(), debug::hit::LineLimit,
        tag.shot, tag.timestamp, tickIndex, tickIndex - tag.tick, line);
}
}

void arm(Player& player, unsigned shots) {
    std::lock_guard lock(mutex);
    budget.arm(shots); history.clear(); scope = shots ? &player.getLevel() : nullptr;
    owner = player.mLegacyUniqueID->rawID; armedAt = tickIndex;
}
void clear() {
    std::lock_guard lock(mutex);
    budget.arm(0); history.clear(); scope = nullptr; owner = 0;
}
void tick(Level& level) {
    std::lock_guard lock(mutex);
    ++tickIndex;
    if (!scope) return;
    if (scope != &level || tickIndex - armedAt >= debug::hit::TickLimit || !budget.available()) {
        budget.arm(0); scope = nullptr; history.clear(); return;
    }
    // At the existing PRE-subTick bullet checkpoint. Native actor positions are
    // read, never written. Retain at most two 4096-actor maps while armed.
    debug::hit::PositionHistory::Samples positions;
    auto sample = [&](Actor& actor) {
        if (positions.size() < 4096 && actor.hasCategory(ActorCategory::Mob))
            positions.emplace(actor.mLegacyUniqueID->rawID, fromNative(actor.getPosition()));
    };
    level.forEachPlayer([&](Player& player) { sample(player); return true; });
    for (auto const& entity : level.getEntities()) if (auto actor = entity.tryUnwrap()) sample(*actor);
    history.sample(tickIndex, std::move(positions));
}
debug::hit::Tag trigger(Player& player, int64_t timestamp) {
    std::lock_guard lock(mutex);
    if (scope != &player.getLevel() || owner != player.mLegacyUniqueID->rawID) return {};
    return budget.trigger(tickIndex, timestamp).value_or(debug::hit::Tag{});
}
void spawn(Player& shooter, debug::hit::Tag tag, int serial, int pellet, glm::dvec3 origin,
    glm::dvec3 direction, glm::dvec3 velocity) {
    if (!tag.generation) return;
    std::lock_guard lock(mutex);
    if (!scope || !budget.accepts(tag)) return;
    auto current = fromNative(shooter.getPosition());
    auto previous = history.previousPosition(shooter.mLegacyUniqueID->rawID, tickIndex);
    auto nativePrevious = fromNative(*shooter.mBuiltInComponents->mStateVectorComponent->mPosPrev);
    auto const& rotation = shooter.getRotation();
    log(tag, fmt::format("phase=spawn bullet={} pellet={} shooter={} pos={} previousKnown={} previous={} nativePrevious={} origin={} javaOriginEstimate={} rotation=({:.6f},{:.6f}) direction={} posDelta={} velocity={}",
        serial, pellet, shooter.mLegacyUniqueID->rawID, vec(current), previous.has_value(),
        previous ? vec(*previous) : "unknown", vec(nativePrevious), vec(origin),
        previous ? vec(debug::hit::midpointEye(origin, current, *previous)) : "unknown", rotation.x, rotation.z,
        vec(direction), vec(fromNative(shooter.getPosDelta())), vec(velocity)));
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
    auto fixed = gun::fixedHitbox(target);
    auto current = fromNative(actor.getPosition());
    auto previous = history.previousPosition(actor.mLegacyUniqueID->rawID, tickIndex);
    auto measured = target;
    if (previous) measured.velocity = current - *previous;
    auto measuredFixed = gun::fixedHitbox(measured);
    log(tag, fmt::format("phase=candidate bullet={} age={} target={} runtime={} type={} pos={} previousKnown={} previous={} nativePrevious={} raw={} posDelta={} measured={} fixedDelta={} fixedMeasured={} distanceRaw={:.6f} distanceDelta={:.6f} distanceMeasured={} hitRaw={} hitDelta={} hitMeasured={} player={} crouching={} riding={}",
        serial, age, actor.mLegacyUniqueID->rawID, actor.getRuntimeID().rawID, actor.getTypeName(), vec(current),
        previous.has_value(), previous ? vec(*previous) : "unknown", vec(fromNative(*actor.mBuiltInComponents->mStateVectorComponent->mPosPrev)),
        box(target.box), vec(target.velocity), previous ? vec(measured.velocity) : "unknown", box(fixed),
        previous ? box(measuredFixed) : "unknown", debug::hit::segmentBoxDistance(target.box, from, end),
        debug::hit::segmentBoxDistance(fixed, from, end),
        previous ? fmt::format("{:.6f}", debug::hit::segmentBoxDistance(measuredFixed, from, end)) : "unknown",
        gun::clip(target.box, from, end).has_value(), gun::clip(fixed, from, end).has_value(),
        previous ? (gun::clip(measuredFixed, from, end) ? "true" : "false") : "unknown",
        target.isPlayer, target.isCrouching, target.isRiding));
}
} // namespace tacz::server::hitdiag
