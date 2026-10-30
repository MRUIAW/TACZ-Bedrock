#include "server/BulletSystem.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <glm/glm.hpp>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/server/ServerStoppingEvent.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "ll/api/io/Logger.h"

#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec3.h"
#include "mc/deps/ecs/gamerefs_entity/GameRefsEntity.h"
#include "mc/deps/shared_types/legacy/actor/ActorDamageCause.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/server/commands/CommandUtils.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/ActorCategory.h"
#include "mc/world/actor/ActorDamageByChildActorSource.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorHurtResult.h"
#include "mc/world/actor/HurtParameters.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/ShapeType.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/phys/AABB.h"
#include "mc/world/phys/HitResult.h"
#include "mc/world/phys/HitResultType.h"

#include "debug/RenderDebug.h"
#include "gun/BulletHit.h"
#include "gun/BulletPhysics.h"
#include "gun/GunStats.h"
#include "gun/ShootRules.h"
#include "mod/TaczMod.h"
#include "net/DebugPackets.h"
#include "net/HitFeedbackPacket.h"
#include "server/HitDiagnostic.h"
#include "server/HitInput.h"

namespace tacz::server::bullet {
namespace {

using Cause = SharedTypes::Legacy::ActorDamageCause;

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

struct Bullet {
    ActorUniqueID     entity;
    ActorUniqueID     shooter;
    gun::BulletMotion motion;
    glm::dvec3        origin;
    gun::BulletDamage damage;
    int               pierce = 1;
    float             knockback = 0;
    bool              igniteEntity = false;
    int               igniteSeconds = 0;
    int               serial = 0;
    pack::ResourceId  gunId;
    debug::hit::Tag diagnostic;
};

std::mutex          mutex;
std::vector<Bullet> bullets;
int                 nextSerial = 1;
gun::DisplacementHistory targetHistory;
Level const* historyLevel = nullptr;

std::array<ll::event::ListenerPtr, 2> listeners;

Vec3       toVec3(glm::dvec3 v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
glm::dvec3 toGlm(Vec3 const& v) { return {v.x, v.y, v.z}; }

void place(Actor& actor, gun::BulletMotion const& motion) {
    auto rot = motion.rotation();
    if (debug::bulletMoveMode == 1) {
        actor.teleport(toVec3(motion.pos), actor.getDimensionId(), Vec2{rot.x, rot.y});
    } else {
        actor.moveTo(toVec3(motion.pos), Vec2{rot.x, rot.y});
    }
}

// TACZ splits each hit into an armour-reduced part and an armour-piercing part. Bedrock has no
// bullet damage type, so the piercing part uses the first cause the game does not reduce by
// armour; it is picked once at runtime instead of assuming which causes bypass armour.
Cause piercingCause() {
    static std::optional<Cause> chosen;
    if (!chosen) {
        for (Cause candidate : {Cause::Magic, Cause::Override, Cause::Void}) {
            ActorDamageSource source(candidate, {});
            if (!source.isReducedByArmorReduction()) {
                chosen = candidate;
                break;
            }
        }
        if (!chosen) {
            chosen = Cause::Projectile;
            logger().warn("No damage cause bypasses armour; armour_ignore has no effect");
        }
        logger().info("Armour-piercing bullet damage uses cause {}", static_cast<int>(*chosen));
    }
    return *chosen;
}

// TACZ BlockRayTrace: blocks tagged tacz:bullet_ignore (iron bars, fences, fence gates, leaves)
// let bullets through. Bedrock has no block tags, so the tag is matched by name.
bool bulletPassesThrough(Block const& block) {
    std::string_view name = block.getTypeName();
    if (auto colon = name.find(':'); colon != std::string_view::npos) name.remove_prefix(colon + 1);
    return name == "iron_bars" || name.ends_with("fence") || name.ends_with("fence_gate")
        || name.find("leaves") != std::string_view::npos;
}

// One hurt() call. TACZ clears the target's hurt cooldown before each part so full-auto fire
// is not swallowed by invulnerability frames.
bool applyDamage(Actor& target, Actor const& bulletActor, Actor const* shooter, float amount, Cause cause, bool knockback) {
    if (amount <= 0) return false;
    target.mInvulnerableTime = 0;
    target.mLastHurt         = 0;
    HurtParameters params(knockback, false);
    if (shooter) {
        ActorDamageByChildActorSource source(bulletActor, *shooter, cause);
        return static_cast<bool>(target.hurt(source, amount, params));
    }
    ActorDamageSource source(cause, {});
    return static_cast<bool>(target.hurt(source, amount, params));
}

// TACZ EntityUtil PROJECTILE_TARGETS plus the owner / same-vehicle exclusions. Bedrock has no
// isPickable, so only mobs and players are targets for now.
bool isTarget(Actor& candidate, Actor const& bulletActor, Actor const* shooter) {
    if (&candidate == &bulletActor || &candidate == shooter) return false;
    if (candidate.getTypeName() == "tacz:bullet") return false;
    if (!candidate.hasCategory(ActorCategory::Mob)) return false;
    if (candidate.isDead() || !candidate.isAlive() || candidate.isSpectator()) return false;
    if (shooter) {
        Actor* vehicle = shooter->getVehicle();
        if (vehicle && (&candidate == vehicle || candidate.getVehicle() == vehicle)) return false;
    }
    return true;
}

struct EntityHit {
    Actor*     actor;
    glm::dvec3 pos;
    double     distance; // from the segment start, for ordering
    bool       headshot;
};

std::vector<EntityHit> findEntityHits(
    BlockSource& region,
    Actor const& bulletActor,
    Actor const* shooter,
    glm::dvec3   from,
    glm::dvec3   to,
    Bullet const& bullet
) {
    std::vector<EntityHit> hits;
    // TACZ: bullet box expanded along the motion, inflated by 1.
    glm::dvec3 lo = glm::min(from, to) - 1.0;
    glm::dvec3 hi = glm::max(from, to) + 1.0;
    for (Actor* candidate : region.getEntities(AABB{toVec3(lo), toVec3(hi)}, 1.0f)) {
        if (!candidate || !isTarget(*candidate, bulletActor, shooter)) continue;
        AABB const&       aabb = candidate->getAABB();
        gun::HitboxTarget target{
            {toGlm(aabb.min), toGlm(aabb.max)},
            targetHistory.velocity(candidate->getOrCreateUniqueID().rawID, toGlm(candidate->getPosition()), candidate->getDimensionId()),
            candidate->isPlayer(),
            candidate->isSneaking(),
            candidate->getVehicle() != nullptr,
        };
        auto pos = gun::clip(gun::fixedHitbox(target), from, to);
        hitdiag::candidate(bullet.diagnostic, bullet.serial, bullet.motion.age, *candidate, target, from, to);
        if (!pos) continue;
        double eyeY = candidate->getHeadPos().y;
        hits.push_back({candidate, *pos, glm::length(*pos - from), gun::isHeadshot(pos->y, eyeY)});
    }
    std::ranges::sort(hits, {}, &EntityHit::distance);
    return hits;
}

void reportHit(Actor const* shooter, std::string const& info) {
    logger().info("{}", info);
    // Shown on the shooter's probe HUD ("server: ...") as in-game feedback.
    if (shooter && shooter->isPlayer()) net::DebugAckPacket{0, info}.sendTo(*static_cast<Player const*>(shooter));
}

void hitEntity(Bullet& b, Actor& bulletActor, Actor* shooter, EntityHit const& hit) {
    Actor& target = *hit.actor;
    double fromMuzzle = glm::length(hit.pos - b.origin);
    auto   split      = b.damage.onHit(fromMuzzle, hit.headshot);
    int    before     = target.getHealth();
    bool   knockback  = b.knockback > 0;
    bool   normal     = applyDamage(target, bulletActor, shooter, split.normal, Cause::Projectile, knockback);
    bool   piercing   = applyDamage(target, bulletActor, shooter, split.armorPiercing, piercingCause(), false);
    if (b.igniteEntity && b.igniteSeconds > 0) target.setOnFire(b.igniteSeconds, true);
    int  after  = target.getHealth();
    bool killed = target.isDead() || !target.isAlive();
    if ((normal || piercing) && shooter && shooter->isPlayer())
        net::HitFeedbackPacket{b.gunId.toString(),hit.headshot,killed}.sendTo(*static_cast<Player const*>(shooter));

    AABB const& aabb = target.getAABB();
    reportHit(
        shooter,
        fmt::format(
            "[bullet #{}] hit {}{} at {:.1f} m: {:.2f} + {:.2f} piercing (applied {}/{}), hp {} -> {}{} "
            "| hit y {:.2f} above feet, eye {:.2f}",
            b.serial,
            target.getTypeName(),
            hit.headshot ? " (HEAD)" : "",
            fromMuzzle,
            split.normal,
            split.armorPiercing,
            normal,
            piercing,
            before,
            after,
            killed ? " KILLED" : "",
            hit.pos.y - aabb.min.y,
            target.getHeadPos().y - aabb.min.y
        )
    );
}

// Advances one bullet by a tick. Returns false when it should be removed.
bool tick(Level& level, Bullet& b, std::string& reason) {
    Actor* actor = level.fetchEntity(b.entity, false);
    if (!actor) {
        reason = "entity gone";
        return false;
    }
    Actor* shooter = level.fetchEntity(b.shooter, false);
    auto&  region  = actor->getDimensionBlockSource();
    auto   from    = b.motion.pos;
    auto   to      = b.motion.next();
    if (!region.hasChunksAt(BlockPos{toVec3(to)}, 0, false)) {
        reason = "unloaded chunk";
        actor->remove();
        return false;
    }
    // Blocks first (collision shape, fluids ignored: TACZ ClipContext COLLIDER / NONE); entities
    // are then only looked for up to the block hit.
    double distance = glm::length(to - from);
    auto   blockHit = region.clip(
        toVec3(from),
        toVec3(to),
        false,
        ShapeType::Collision,
        (static_cast<int>(distance) + 1) * 2,
        false,
        false,
        nullptr,
        [](BlockSource const&, Block const& block, bool) { return !bulletPassesThrough(block); },
        false
    );
    bool hitBlock = blockHit.mType == HitResultType::Tile;
    auto end      = hitBlock ? toGlm(blockHit.mPos) : to;
    hitdiag::segment(b.diagnostic, b.serial, b.motion.age, from, end, to, hitBlock);

    for (auto const& hit : findEntityHits(region, *actor, shooter, from, end, b)) {
        hitEntity(b, *actor, shooter, hit);
        if (--b.pierce < 1) {
            b.motion.pos = hit.pos;
            place(*actor, b.motion);
            reason = "stopped in " + hit.actor->getTypeName();
            actor->remove();
            return false;
        }
    }
    if (hitBlock) {
        b.motion.pos = end;
        place(*actor, b.motion);
        reason = fmt::format("hit block ({}, {}, {})", blockHit.mBlock.x, blockHit.mBlock.y, blockHit.mBlock.z);
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
    if (historyLevel != &level) { targetHistory.clear(); historyLevel = &level; }
    // Snapshot this PRE checkpoint before damage can move/kill a target; keep
    // the PRIOR checkpoint available until all bullets have been tested.
    gun::DisplacementHistory::Samples positions;
    auto sample = [&](Actor& actor) {
        if (positions.size() < gun::DisplacementHistory::Limit && actor.hasCategory(ActorCategory::Mob)
            && actor.isAlive() && !actor.isDead())
            positions.emplace(actor.getOrCreateUniqueID().rawID,
                gun::DisplacementHistory::Position{toGlm(actor.getPosition()), actor.getDimensionId()});
    };
    level.forEachPlayer([&](Player& player) { sample(player); return true; });
    for (auto const& entity : level.getEntities()) if (auto actor = entity.tryUnwrap()) sample(*actor);
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
    // Whole-map replacement drops despawned actors at the next checkpoint;
    // getOrCreateUniqueID avoids sharing the unassigned legacy sentinel (-1).
    targetHistory.sample(std::move(positions));
}

} // namespace

void fire(Player& shooter,pack::ResourceId const& gunId, pack::GunData const& gun, std::optional<pack::FireMode> mode, float inaccuracy, debug::hit::Tag diagnostic) {
    static std::mt19937 rng{std::random_device{}()};

    // Resolve afresh per round, not once per trigger/task. Raw auth input has
    // already arrived but Bedrock has not necessarily applied its queued motion.
    auto input = hitinput::snapshot(shooter);
    auto rotation = shooter.getRotation();
    double eyeHeight = gun::javaEyeHeight(shooter.isSneaking(), shooter.isSwimming(), shooter.isGliding(), shooter.isCrawling());
    auto aim = gun::roundAim(input, toGlm(shooter.getFeetPos()), {rotation.x, rotation.z}, eyeHeight);
    auto   origin    = aim.origin;
    auto   direction = aim.direction;
    double speed     = gun::bulletSpeedPerTick(gun, mode) * debug::bulletSpeedScale.load();
    auto   damage    = gun::BulletDamage::from(gun, mode);
    int    pellets   = std::max(gun.bullet.bulletAmount, 1);
    // Projectile.shootFromRotation: the bullet inherits the shooter's motion (vertical only in the air).
    auto inherited = toGlm(shooter.getPosDelta());
    if (shooter.isOnGround()) inherited.y = 0;

    for (int i = 0; i < pellets; ++i) {
        auto velocity = gun::spreadVelocity(direction, speed, inaccuracy, rng) + inherited;
        auto motion   = gun::BulletMotion::launchWithVelocity(gun.bullet, origin, velocity);
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
        Bullet b;
        b.entity        = actor->getOrCreateUniqueID();
        b.shooter       = shooter.getOrCreateUniqueID();
        b.motion        = motion;
        b.origin        = origin;
        b.damage        = damage;
        b.pierce        = std::max(gun.bullet.pierce, 1);
        b.knockback     = gun.bullet.knockback;
        b.igniteEntity  = gun.bullet.ignite.entity;
        b.igniteSeconds = gun.bullet.igniteEntityTime;
        b.serial        = nextSerial++;
        b.gunId         = gunId;
        b.diagnostic    = diagnostic;
        hitdiag::spawn(shooter, diagnostic, b.serial, i, origin, direction, velocity, input);
        bullets.push_back(std::move(b));
        logger().info(
            "[bullet #{}] fired at {:.2f} blocks/tick (x{:.2f}), inaccuracy {:.2f}, life {} ticks, damage {:.1f} at the muzzle",
            bullets.back().serial,
            glm::length(motion.vel),
            debug::bulletSpeedScale.load(),
            inaccuracy,
            motion.life,
            damage.atDistance(0)
        );
    }
}

void enable() {
    auto& bus    = ll::event::EventBus::getInstance();
    listeners[0] = bus.emplaceListener<ll::event::ServerLevelTickEvent>([](ll::event::ServerLevelTickEvent& event) {
        hitdiag::tick(event.level());
        tickAll(event.level());
    });
    // The world is going away: its entities, and with them our handles, are no longer valid.
    listeners[1] = bus.emplaceListener<ll::event::ServerStoppingEvent>([](ll::event::ServerStoppingEvent&) {
        std::lock_guard lock(mutex);
        bullets.clear();
        targetHistory.clear(); historyLevel = nullptr;
        hitdiag::clear();
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
    targetHistory.clear(); historyLevel = nullptr;
    hitdiag::clear();
}

} // namespace tacz::server::bullet
