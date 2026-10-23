#include "client/animation/GunAnimation.h"

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

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
#include "client/sound/GunSound.h"
#include "lua/AnimationStateMachine.h"
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
    lua::AnimationContext context;
    pack::GunEntry const* entry = nullptr;
    animation::AnimationFile const* clips = nullptr;
    std::unique_ptr<lua::AnimationStateMachine> machine;
    bool loggedHidden = false;
    bool loggedShell = false;
};

// VM/cache belong exclusively to the client frame thread. Packets only enqueue requests;
// the renderer reads immutable published poses under the snapshot lock.
std::map<pack::ResourceId, animation::AnimationFile> cache;
std::optional<pack::ResourceId> currentGun;
int currentSlot = -1;
std::unique_ptr<Session> session;
std::mutex mutex;
std::optional<std::pair<std::string, std::string>> request;
Snapshot published;
std::optional<pack::ResourceId> publishedGun;

nlohmann::json readNbt(std::vector<std::string> const& path, std::string const& key, int mode) {
    auto client = ll::service::getClientInstance();
    auto player = client ? client->getLocalPlayer() : nullptr;
    if (!player || path.size() > 64) return {};
    auto tag = player->getSelectedItem().mUserData.get();
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
    auto gun = nbt::readGun(player->getSelectedItem());
    if (!gun || gun->gunId != currentGun) return;
    auto& c = session->context;
    auto const& data = session->entry->data;
    c.ammo = gun->ammo;
    c.barrel = data.bolt != pack::Bolt::OpenBolt && gun->barrel;
    c.maxAmmo = data.ammoAmount;
    c.magExtent = 0; // attachments are not implemented, not a guessed extension level
    c.ammoToConsume = player->isCreative() || data.reload.infinite || (data.ammoId && nbt::countAmmo(*player, *data.ammoId) > 0);
    auto timing = shooter::animationTiming();
    c.fireMode = timing.fireMode;
    c.shootInterval = timing.interval;
    c.shootCooldown = timing.cooldown;
    c.lastShootTimestamp = timing.lastShootTimestamp;
    c.timestamp = timestamp();
    c.reloadState = static_cast<int>(shooter::reloadState());
    c.putAwayTime = data.putAwayTime;
    c.partialTicks = client->getFrameAlpha();
    c.crouching = player->isSneaking();
    c.movingSlowly = c.crouching; // Java isMovingSlowly: crouching or crawling (crawl not implemented)
    c.sprinting = player->isSprinting();
    c.onGround = player->isOnGround();
    auto& entity = player->getEntityContext();
    c.walkDist = ActorMovement::getPredictedWalkDist(entity, c.partialTicks);
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
        for (auto const& key : keys) if (key.time >= a && key.time <= b) sound::play(key.sound, 1.f, 1.f);
    };
    if (from > to && from <= keys.back().time) {
        range(0, to);
        to = keys.back().time;
    }
    range(from, to);
}

void trigger(std::string const& input) {
    if (!session || !session->machine) return;
    refresh();
    session->machine->setTime(nowSeconds());
    session->machine->trigger(input);
}

class AnimationDebugHandler final : public ll::network::PacketHandlerBase<AnimationDebugHandler, net::AnimationDebugPacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, net::AnimationDebugPacket const& packet) const {
        std::lock_guard lock(mutex);
        request = std::pair{packet.gunId, packet.name};
    }
};
} // namespace

bool held(std::optional<pack::ResourceId> const& gun, int slot) {
    if (gun == currentGun && slot == currentSlot) { refresh(); return false; }
    if (session && session->machine) {
        session->machine->setTime(nowSeconds());
        session->machine->trigger("put_away");
        session->machine->exit();
        // Bedrock selection has already changed. Delaying the actual inventory switch is
        // deferred; dispatch put_away but do not display the old gun over the new item.
    }
    session.reset();
    currentGun = gun;
    currentSlot = slot;
    if (!gun) return true;
    auto const& packs = TaczMod::getInstance().gunPacks();
    auto entry = packs.gun(*gun);
    if (!entry || !entry->index.display) return true;
    auto display = packs.gunDisplay(*entry->index.display);
    if (!display) return true;
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
    logger().info("[anim] {}: Lua {}, {} clips", gun->toString(), script.toString(), session->clips->size());
    return true;
}

void update() {
    std::optional<std::pair<std::string, std::string>> debug;
    { std::lock_guard lock(mutex); debug.swap(request); }
    if (debug && session && currentGun && debug->first == currentGun->toString()) {
        if (!session->machine->playDebug(debug->second, nowSeconds())) {
            std::string names;
            for (auto const& [name, clip] : *session->clips) names += (names.empty() ? "" : ", ") + name;
            logger().info("[anim] Unknown '{}'. Available: {}", debug->second, names);
        } else logger().info("[anim] Debug {} once, then return to Lua", debug->second);
    }
    animation::Pose pose;
    if (session) {
        refresh();
        pose = session->machine->update(nowSeconds());
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
    if (pose != published.pose || currentGun != publishedGun) {
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
    refresh();
    if (!session) return;
    auto const& c = session->context;
    trigger(std::string(lua::movementInput(c)));
}

Snapshot snapshot(pack::ResourceId const& gun) {
    std::lock_guard lock(mutex);
    return publishedGun && *publishedGun == gun ? published : Snapshot{};
}

void clear() {
    held(std::nullopt);
    cache.clear();
    std::lock_guard lock(mutex);
    request.reset();
    published.pose.clear();
    ++published.revision;
    publishedGun.reset();
}

} // namespace tacz::client::anim
