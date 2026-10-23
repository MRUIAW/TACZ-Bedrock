#include "server/BulletSystem.h"

#include <array>
#include <mutex>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/io/Logger.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/server/commands/CommandUtils.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/ShapeType.h"
#include "mc/world/phys/HitResult.h"
#include "mc/world/phys/HitResultType.h"

#include "debug/RenderDebug.h"
#include "gun/BulletPhysics.h"
#include "mod/TaczMod.h"

namespace tacz::server::bullet {
namespace {

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

struct Bullet {
    ActorUniqueID     entity;
    gun::BulletMotion motion;
    glm::dvec3        origin;
    int               serial;
};

std::mutex          mutex;
std::vector<Bullet> bullets;
int                 nextSerial = 1;

std::array<ll::event::ListenerPtr, 2> listeners;

Vec3 toVec3(glm::dvec3 v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
glm::dvec3 toGlm(Vec3 const& v) { return {v.x, v.y, v.z}; }

void place(Actor& actor, gun::BulletMotion const& motion) {
    auto rot = motion.rotation();
    if (debug::bulletMoveMode == 1) {
        actor.teleport(toVec3(motion.pos), actor.getDimensionId(), Vec2{rot.x, rot.y});
    } else {
        actor.moveTo(toVec3(motion.pos), Vec2{rot.x, rot.y});
    }
}

// Advances one bullet by a tick. Returns false when it should be removed.
bool tick(Level& level, Bullet& b, std::string& reason) {
    Actor* actor = level.fetchEntity(b.entity, false);
    if (!actor) {
        reason = "entity gone";
        return false;
    }
    auto& region = actor->getDimensionBlockSource();
    auto  from   = b.motion.pos;
    auto  to     = b.motion.next();
    if (!region.hasChunksAt(BlockPos{toVec3(to)}, 0, false)) {
        reason = "unloaded chunk";
        actor->remove();
        return false;
    }
    // TACZ ray-traces blocks with the collision shape and ignores fluids (ClipContext COLLIDER/NONE).
    double distance = glm::length(to - from);
    auto   hit      = region.clip(
        toVec3(from),
        toVec3(to),
        false,
        ShapeType::Collision,
        (static_cast<int>(distance) + 1) * 2,
        false,
        false,
        nullptr,
        [](BlockSource const&, Block const&, bool) { return true; },
        false
    );
    if (hit.mType == HitResultType::Tile) {
        b.motion.pos = toGlm(hit.mPos);
        place(*actor, b.motion);
        reason = "hit block (" + std::to_string(hit.mBlock.x) + ", " + std::to_string(hit.mBlock.y) + ", "
               + std::to_string(hit.mBlock.z) + ")";
        actor->remove();
        return false;
    }
    bool alive = b.motion.step();
    place(*actor, b.motion);
    if (!alive) {
        reason = "expired";
        actor->remove();
    }
    return alive;
}

void tickAll(Level& level) {
    std::lock_guard lock(mutex);
    for (auto it = bullets.begin(); it != bullets.end();) {
        std::string reason;
        if (tick(level, *it, reason)) {
            ++it;
            continue;
        }
        logger().info(
            "[bullet #{}] {} after {} ticks, {:.1f} blocks from the muzzle",
            it->serial,
            reason,
            it->motion.age,
            glm::length(it->motion.pos - it->origin)
        );
        it = bullets.erase(it);
    }
}

} // namespace

void fire(Player& shooter, pack::BulletData const& data) {
    pack::BulletData scaled = data;
    scaled.speed *= debug::bulletSpeedScale.load();

    auto origin    = toGlm(shooter.getHeadPos());
    auto direction = toGlm(shooter.getViewVector());
    auto motion    = gun::BulletMotion::launch(scaled, origin, direction);

    ActorUniqueID id{};
    Actor*        actor = CommandUtils::spawnEntityAt(
        shooter.getDimensionBlockSource(),
        toVec3(origin),
        ActorDefinitionIdentifier("tacz:bullet"),
        id,
        &shooter
    );
    if (!actor) {
        logger().warn("Cannot spawn tacz:bullet; is the TACZ behavior pack loaded?");
        return;
    }
    place(*actor, motion);

    std::lock_guard lock(mutex);
    int serial = nextSerial++;
    bullets.push_back({actor->getOrCreateUniqueID(), motion, origin, serial});
    logger().info(
        "[bullet #{}] fired at {:.2f} blocks/tick (x{:.2f}), life {} ticks, move mode {}",
        serial,
        glm::length(motion.vel),
        debug::bulletSpeedScale.load(),
        motion.life,
        debug::bulletMoveMode.load()
    );
}

void enable() {
    auto& bus    = ll::event::EventBus::getInstance();
    listeners[0] = bus.emplaceListener<ll::event::ServerLevelTickEvent>([](ll::event::ServerLevelTickEvent& event) {
        tickAll(event.level());
    });
    // The world is going away: its entities, and with them our handles, are no longer valid.
    listeners[1] = bus.emplaceListener<ll::event::ServerStoppingEvent>([](ll::event::ServerStoppingEvent&) {
        std::lock_guard lock(mutex);
        bullets.clear();
    });
}

void disable() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : listeners) {
        if (listener) bus.removeListener(listener);
        listener = nullptr;
    }
    std::lock_guard lock(mutex);
    bullets.clear();
}

} // namespace tacz::server::bullet
