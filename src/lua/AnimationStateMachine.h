#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "model/AnimationController.h"
#include "pack/GunPackLoader.h"

namespace tacz::lua {

// GunAnimationStateContext / ItemAnimationStateContext inputs. Minecraft only fills this
// snapshot; the VM, controller and tests have no mc/ or LeviLamina dependency.
struct AnimationContext {
    int ammo = 0, maxAmmo = 0, magExtent = 0, fireMode = 0, reloadState = 0;
    bool barrel = false, ammoToConsume = false;
    int64_t shootInterval = 0, shootCooldown = 0, lastShootTimestamp = -1, timestamp = 0;
    float partialTicks = 0, putAwayTime = 0, walkDist = 0, walkAnchor = 0;
    bool inputUp = false, inputDown = false, inputLeft = false, inputRight = false, inputJumping = false;
    bool inputAvailable = false;
    float moveMagnitude = 0;
    bool onGround = false, crouching = false, sprinting = false, movingSlowly = false;
    bool hideCrossHair = false;
    std::vector<int> shells; // requested ejection windows; rendering deferred
    nlohmann::json params = nlohmann::json::object();
    std::function<void(int64_t)> adjustShootInterval;
    animation::LogFn print;
    // path, key, mode: mode 0 reads the NBT tag type, mode 1 its primitive value.
    std::function<nlohmann::json(std::vector<std::string> const&, std::string const&, int)> readNbt;
};

// TickAnimationEvent's input classification (including missing input and slow movement).
std::string_view movementInput(AnimationContext const& context);

class AnimationStateMachine {
public:
    AnimationStateMachine(
        pack::GunPackRegistry const& packs,
        animation::AnimationFile const& animations,
        pack::ResourceId script,
        AnimationContext& context,
        animation::LogFn warn = {},
        animation::Controller::SoundFn sounds = {}
    );
    ~AnimationStateMachine();
    bool initialize(double now);
    bool trigger(std::string const& input);
    void setTime(double now);
    animation::Pose update(double now);
    void exit();
    bool healthy() const;
    animation::Controller& controller();
    // One-shot visual override; the machine continues updating and resumes when it ends.
    bool playDebug(std::string const& name, double now);

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace tacz::lua
