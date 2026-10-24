#include "client/animation/GunAnimation.h"

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "ll/api/io/Logger.h"
#include "mc/deps/core/utility/optional_ref.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/entity/components/MoveInputComponent.h"
#include "mc/world/actor/provider/ActorMovement.h"
#include "mc/world/item/ItemStack.h"

#include "client/gameplay/ClientShooter.h"
#include "client/gameplay/ClientAim.h"
#include "client/sound/GunSound.h"
#include "lua/AnimationStateMachine.h"
#include "gun/DrawRules.h"
#include "model/WalkDistance.h"
#include "mod/TaczMod.h"
#include "nbt/AmmoItem.h"
#include "nbt/GunItem.h"
#include "net/DebugPackets.h"
#include "pack/GunAnimations.h"

namespace tacz::client::anim {
namespace {
using Clock = std::chrono::steady_clock;
double nowSeconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
int64_t timestamp() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

struct Session {
    // Animation owns DATA, not an engine ItemStack/ItemRegistry/slot reference. Native
    // ItemStack's copy ctor resolves the thread-local registry, which is out of scope
    // after MinecraftGame::update returns (0.3.7 world-join crash).
    std::optional<nbt::GunItemState> itemState;
    std::unique_ptr<CompoundTag> itemNbt;
    lua::AnimationContext context;
    pack::GunEntry const* entry = nullptr;
    animation::AnimationFile const* clips = nullptr;
    std::unique_ptr<lua::AnimationStateMachine> machine;
    bool loggedHidden = false;
    bool loggedShell = false;
    std::string measuredTrack;
    double measuredPhase = 0, measuredAt = 0;
    float measuredWalk = 0;
    double measuredCycles = 0, measuredSeconds = 0;
};

// VM/cache belong exclusively to the client frame thread. Packets only enqueue requests;
// the renderer reads immutable published poses under the snapshot lock.
std::map<pack::ResourceId, animation::AnimationFile> cache;
std::optional<pack::ResourceId> currentGun;
gun::HandSwitch hand;
animation::WalkDistance walk;
struct MovementSample {
    double distance;
    float native;
    std::string input;
    uint64_t world;
};
std::vector<MovementSample> movementSamples;
std::mutex movementMutex;
std::optional<glm::dvec2> lastPosition;
float previousNative = 0;
uint64_t movementWorld = 0;
double diagnosticDistance = 0;
float diagnosticNative = 0;
unsigned diagnosticTicks = 0;
double diagnosticStart = 0;
std::unique_ptr<Session> session;
std::mutex mutex;
struct DebugRequest {
    std::string gun, name;
    uint64_t world;
};
std::optional<DebugRequest> request;
Snapshot published;
std::optional<pack::ResourceId> publishedGun;

nlohmann::json readNbt(std::vector<std::string> const& path, std::string const& key, int mode) {
    if (!session || path.size() > 64) return {};
    auto tag = session->itemNbt.get();
    for (auto const& p : path) {
        if (!tag || !tag->contains(p, Tag::Compound)) return {};
        tag = &(*tag)[p].get<CompoundTag>();
    }
    if (!tag || !tag->contains(key)) return {};
    auto const& value = (*tag)[key];
    if (mode == 0) return static_cast<int>(value.index());
    switch (static_cast<Tag::Type>(value.index())) {
    case Tag::Byte: return static_cast<schar>(value.get<ByteTag>().data);
    case Tag::Short: return value.get<ShortTag>().data;
    case Tag::Int: return value.get<IntTag>().data;
    case Tag::Int64: return value.get<Int64Tag>().data;
    case Tag::Float: return value.get<FloatTag>().data;
    case Tag::Double: return value.get<DoubleTag>().data;
    case Tag::String: return static_cast<std::string const&>(value.get<StringTag>());
    default: return {};
    }
}

void refresh() {
    if (!session || !session->entry) return;
    auto client = ll::service::getClientInstance();
    auto player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return;
    auto gun = hand.puttingAway ? session->itemState : nbt::readGun(player->getSelectedItem());
    if (!gun || gun->gunId != currentGun) return;
    auto& c = session->context;
    auto const& data = session->entry->data;
    c.ammo = gun->ammo;
    c.barrel = data.bolt != pack::Bolt::OpenBolt && gun->barrel;
    c.maxAmmo = data.ammoAmount;
    c.magExtent = 0; // attachments are not implemented, not a guessed extension level
    c.ammoToConsume = player->isCreative() || data.reload.infinite || (data.ammoId && nbt::countAmmo(*player, *data.ammoId) > 0);
    if (!hand.puttingAway) {
        session->itemState = gun;
        auto const& tag = player->getSelectedItem().mUserData;
        session->itemNbt = tag ? tag->clone() : nullptr;
        auto timing = shooter::animationTiming();
        c.fireMode = timing.fireMode;
        c.shootInterval = timing.interval;
        c.shootCooldown = timing.cooldown;
        c.lastShootTimestamp = timing.lastShootTimestamp;
        c.reloadState = static_cast<int>(shooter::reloadState());
    }
    c.timestamp = timestamp();
    auto aiming = aim::snapshot();
    c.aiming = !hand.puttingAway && aiming.gun == currentGun && aiming.aiming;
    c.aimingProgress = !hand.puttingAway && aiming.gun == currentGun ? aiming.progress : 0;
    c.putAwayTime = data.putAwayTime;
    c.partialTicks = client->getFrameAlpha();
    c.crouching = player->isSneaking();
    c.movingSlowly = c.crouching; // Java isMovingSlowly: crouching or crawling (crawl not implemented)
    c.sprinting = player->isSprinting();
    c.onGround = player->isOnGround();
    auto& entity = player->getEntityContext();
    c.walkDist = walk.predicted(c.partialTicks);
    c.inputUp = c.inputDown = c.inputLeft = c.inputRight = c.inputJumping = false;
    c.inputAvailable = false;
    c.moveMagnitude = 0;
    if (auto move = entity.tryGetComponent<MoveInputComponent>()) {
        c.inputAvailable = true;
        auto const& movement = move->mMove.get();
        c.moveMagnitude = std::sqrt(movement.x * movement.x + movement.y * movement.y);
        auto const& raw = move->mRawInputState.get();
        auto flag = [&](MoveInputState::Flag f) { return raw.mFlagValues->test(static_cast<size_t>(f)); };
        c.inputUp = flag(MoveInputState::Flag::Up) || flag(MoveInputState::Flag::UpLeft) || flag(MoveInputState::Flag::UpRight);
        c.inputDown = flag(MoveInputState::Flag::Down) || flag(MoveInputState::Flag::DownLeft) || flag(MoveInputState::Flag::DownRight);
        c.inputLeft = flag(MoveInputState::Flag::Left) || flag(MoveInputState::Flag::UpLeft) || flag(MoveInputState::Flag::DownLeft);
        c.inputRight = flag(MoveInputState::Flag::Right) || flag(MoveInputState::Flag::UpRight) || flag(MoveInputState::Flag::DownRight);
        // Analogue stick/touch input has the same directional meaning as the digital flags.
        auto const& analog = raw.mAnalogMoveVector.get();
        if (!c.inputUp && !c.inputDown) { c.inputUp = analog.y > 0.01f; c.inputDown = analog.y < -0.01f; }
        if (!c.inputLeft && !c.inputRight) { c.inputLeft = analog.x > 0.01f; c.inputRight = analog.x < -0.01f; }
        c.inputJumping = flag(MoveInputState::Flag::JumpDown) || flag(MoveInputState::Flag::JumpInputCurrentlyDown);
    }
}

void playSounds(std::string const& name, double from, double to) {
    if (!session || !session->clips || from == to) return;
    auto it = session->clips->find(name);
    if (it == session->clips->end() || it->second.sounds.empty()) return;
    auto const& keys = it->second.sounds;
    // ObjectAnimationSoundChannel.computeIndex includes an exact lower boundary too.
    auto range = [&](double a, double b) {
        for (auto const& key : keys) if (key.time >= a && key.time <= b) sound::play(key.sound, 1.f, 1.f, true);
    };
    if (from > to && from <= keys.back().time) {
        range(0, to);
        to = keys.back().time;
    }
    range(from, to);
}

void trigger(std::string const& input) {
    if (!session || !session->machine || hand.puttingAway) return;
    refresh();
    session->machine->setTime(nowSeconds());
    session->machine->trigger(input);
}

class AnimationDebugHandler final : public ll::network::PacketHandlerBase<AnimationDebugHandler, net::AnimationDebugPacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, net::AnimationDebugPacket const& packet) const {
        auto world = shooter::worldToken();
        if (!world) return;
        std::lock_guard lock(mutex);
        request = DebugRequest{packet.gunId, packet.name, world};
    }
};
} // namespace

void reveal() {
    sound::stopAnimation();
    session.reset();
    currentGun = hand.visible.gun;
    auto const& gun = currentGun;
    if (!gun) return;
    auto const& packs = TaczMod::getInstance().gunPacks();
    auto entry = packs.gun(*gun);
    if (!entry || !entry->index.display) return;
    auto display = packs.gunDisplay(*entry->index.display);
    if (!display) return;
    auto [it, inserted] = cache.try_emplace(*entry->index.display);
    if (inserted) it->second = pack::loadGunAnimations(packs, *display, [](auto const& msg) { logger().warn("{}", msg); });
    session = std::make_unique<Session>();
    session->entry = entry;
    session->clips = &it->second;
    session->context.params = display->stateMachineParam;
    session->context.adjustShootInterval = shooter::adjustShootInterval;
    session->context.print = [](auto const& message) { logger().debug("{}", message); };
    session->context.readNbt = readNbt;
    refresh();
    auto script = display->stateMachine.value_or(pack::ResourceId{"tacz", "default_state_machine"});
    session->machine = std::make_unique<lua::AnimationStateMachine>(
        packs, *session->clips, script, session->context,
        [](auto const& msg) { logger().warn("{}", msg); }, playSounds
    );
    session->machine->initialize(nowSeconds());
    session->machine->trigger("draw");
    sound::playGunSound(*gun, "draw", 1.f, 1.f, true);
    logger().info("[anim] {}: Lua {}, {} clips", gun->toString(), script.toString(), session->clips->size());
}

bool held(std::optional<pack::ResourceId> const& gun, int slot) {
    if (slot < 0) { if (session || currentGun) clear(); return false; }
    auto const& packs = TaczMod::getInstance().gunPacks();
    auto next = gun ? packs.gun(*gun) : nullptr;
    auto selection = gun::HandSelection{gun, slot};
    bool changed = selection != hand.selected;
    auto now = static_cast<int64_t>(nowSeconds() * 1000);
    auto event = hand.select(selection, now,
        session ? gun::animationMillis(session->entry->data.putAwayTime) : 0,
        next ? gun::animationMillis(next->data.drawTime) : 0);
    if (event == gun::HandSwitch::Event::PutAway) {
        sound::stopAnimation();
        if (session && session->machine) {
            refresh(); // uses the immutable old item, not the incoming selected slot
            session->machine->setTime(nowSeconds());
            session->machine->trigger("put_away");
            session->machine->exit(); // controller continues updating after states exit
        }
        if (currentGun) sound::playGunSound(*currentGun, "put_away", 1.f, 1.f, true);
        logger().info("[hand] put-away {}: {} ms + 50 ms guard; vanilla hidden",
            currentGun ? currentGun->toString() : "none", hand.putAwayEnd - now);
    }
    if (hand.advance(now) == gun::HandSwitch::Event::Reveal || event == gun::HandSwitch::Event::Reveal) {
        reveal();
        logger().info("[hand] reveal {} slot {}; draw {} ms", gun ? gun->toString() : "vanilla", slot,
            next ? gun::animationMillis(next->data.drawTime) : 0);
    }
    if (!changed) refresh();
    return changed;
}

void update() {
    auto world = shooter::worldToken();
    if (!world) return;
    std::optional<DebugRequest> debug;
    { std::lock_guard lock(mutex); debug.swap(request); }
    if (debug && debug->world == world && !hand.puttingAway && session && currentGun && debug->gun == currentGun->toString()) {
        if (!session->machine->playDebug(debug->name, nowSeconds())) {
            std::string names;
            for (auto const& [name, clip] : *session->clips) names += (names.empty() ? "" : ", ") + name;
            logger().info("[anim] Unknown '{}'. Available: {}", debug->name, names);
        } else logger().info("[anim] Debug {} once, then return to Lua", debug->name);
    }
    animation::Pose pose;
    if (session) {
        refresh();
        auto now = nowSeconds();
        pose = session->machine->update(now);
        auto& controller = session->machine->controller();
        auto track = controller.info(controller.getTrack(2, 0));
        auto clip = session->clips->find(track.name);
        if (!hand.puttingAway && !track.transitioning && !track.stopped && clip != session->clips->end()
            && clip->second.duration > 0 && (track.name == "run" || track.name.starts_with("walk_"))) {
            double phase = track.progress / clip->second.duration;
            if (session->measuredTrack == track.name) {
                double delta = phase - session->measuredPhase;
                // Unwrap using displacement, allowing multiple complete cycles at low FPS.
                delta += std::round((session->context.walkDist - session->measuredWalk) / 2 - delta);
                session->measuredCycles += delta;
                session->measuredSeconds += now - session->measuredAt;
            }
            session->measuredTrack = track.name;
            session->measuredPhase = phase;
            session->measuredWalk = session->context.walkDist;
            session->measuredAt = now;
        } else session->measuredTrack.clear();
        auto& c = session->context;
        if (c.hideCrossHair != session->loggedHidden) {
            logger().info("[anim] shouldHideCrossHair={} (stored; crosshair rendering deferred)", c.hideCrossHair);
            session->loggedHidden = c.hideCrossHair;
        }
        if (!c.shells.empty()) {
            if (!session->loggedShell) logger().info("[anim] popShellFrom({}) requested (shell rendering deferred)", c.shells.front());
            session->loggedShell = true;
            c.shells.clear();
        }
    }
    pose.erase("camera");
    pose.erase("constraint");
    std::lock_guard lock(mutex);
    ++published.frame;
    if (pose != published.pose || currentGun != publishedGun || hand.puttingAway != published.puttingAway || published.world != world) {
        published.world = world;
        published.gun = currentGun;
        published.puttingAway = hand.puttingAway;
        published.pose = std::move(pose);
        ++published.revision;
        publishedGun = currentGun;
    }
}

void shoot() { trigger("shoot"); }
void reload() { trigger("reload"); }
void cancelReload() { trigger("cancel_reload"); }
void inspect() { trigger("inspect"); }
void fireSelect() { trigger("fire_select"); }
void movementTick() {
    std::vector<MovementSample> samples;
    { std::lock_guard lock(movementMutex); samples.swap(movementSamples); }
    // Consume each tick's displacement, not N copies of the last frame's sample.
    for (auto const& sample : samples) {
        if (sample.world != shooter::worldToken()) continue;
        walk.tick(sample.distance);
        diagnosticDistance += sample.distance;
        diagnosticNative += sample.native;
        ++diagnosticTicks;
        refresh();
        if (session && !hand.puttingAway) {
            session->machine->setTime(nowSeconds());
            session->machine->trigger(sample.input);
        }
    }
    auto now = nowSeconds();
    if (!diagnosticStart) diagnosticStart = now;
    if (now - diagnosticStart >= 1 && diagnosticTicks) {
        double elapsed = now - diagnosticStart;
        if (session && diagnosticDistance > .01) logger().info(
            "[walk-rate] {} speed={:.3f} blocks/s ticks={} javaDelta={:.5f}/tick nativeDelta={:.5f}/tick distancePhase={:.4f} controllerRate={:.4f} cycles/s sampled={:.2f}s",
            session->context.sprinting ? "run" : "walk", diagnosticDistance / elapsed, diagnosticTicks,
            diagnosticDistance * .6 / diagnosticTicks, diagnosticNative / diagnosticTicks, diagnosticDistance * .3 / elapsed,
            session->measuredSeconds > 0 ? session->measuredCycles / session->measuredSeconds : 0, session->measuredSeconds);
        if (session) session->measuredCycles = session->measuredSeconds = 0;
        diagnosticStart = now;
        diagnosticTicks = 0;
        diagnosticDistance = diagnosticNative = 0;
    }
}

void captureMovementTick() {
    auto world = shooter::worldToken();
    if (!world) return;
    auto client = ll::service::getClientInstance();
    auto player = client ? client->getLocalPlayer() : nullptr;
    if (!player) return;
    auto const& pos = player->getPosition();
    glm::dvec2 position{pos.x, pos.z};
    auto native = ActorMovement::getPredictedWalkDist(player->getEntityContext(), 0);
    // Capture the classification on this tick too, rather than replaying the last frame's
    // input N times. No Lua is called from the tick callback.
    lua::AnimationContext movement;
    movement.sprinting = player->isSprinting();
    movement.movingSlowly = player->isSneaking();
    if (auto move = player->getEntityContext().tryGetComponent<MoveInputComponent>()) {
        movement.inputAvailable = true;
        auto const& m = move->mMove.get();
        movement.moveMagnitude = std::hypot(m.x, m.y);
    }
    std::lock_guard lock(movementMutex);
    if (world != shooter::worldToken()) return;
    if (movementWorld != world) {
        movementSamples.clear();
        lastPosition.reset();
        movementWorld = world;
    }
    if (lastPosition) movementSamples.push_back({glm::length(position - *lastPosition), native - previousNative,
        std::string(lua::movementInput(movement)), world});
    lastPosition = position;
    previousNative = native;
}

bool actionsBlocked() { return hand.blocked(static_cast<int64_t>(nowSeconds() * 1000)); }
bool aimAllowed() { return currentGun && !hand.puttingAway && hand.visible == hand.selected; }

Snapshot snapshot() {
    std::lock_guard lock(mutex);
    return published.world && published.world == shooter::worldToken() ? published : Snapshot{};
}

void clear() {
    sound::stopAnimation();
    session.reset();
    currentGun.reset();
    hand.clear();
    walk = {};
    {
        std::lock_guard lock(movementMutex);
        movementSamples.clear();
        lastPosition.reset();
        previousNative = 0;
        movementWorld = 0;
    }
    diagnosticStart = diagnosticDistance = diagnosticNative = 0;
    diagnosticTicks = 0;
    cache.clear();
    std::lock_guard lock(mutex);
    request.reset();
    published.pose.clear();
    published.gun.reset();
    published.puttingAway = false;
    published.world = 0;
    ++published.revision;
    publishedGun.reset();
}

} // namespace tacz::client::anim
