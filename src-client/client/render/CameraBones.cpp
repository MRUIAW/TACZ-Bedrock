#include "client/render/CameraBones.h"

#include <type_traits>
#include <algorithm>
#include <optional>
#include <chrono>
#include <cmath>
#include <utility>
#include <atomic>
#include <vector>
#include <string>
#include "ll/api/memory/Hook.h"
#include "ll/api/io/Logger.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/options/option_types/Option.h"
#include "mc/client/gui/controls/renderers/HudCursorRenderer.h"
#include "mc/client/gui/screens/InGamePlayScreen.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/minecraft_camera/components/CameraComponent.h"
#include "mc/deps/minecraft_camera/components/RenderCameraComponent.h"
#include "mc/deps/minecraft_camera/CameraClientInstanceComponent.h"
#include "mc/deps/minecraft_camera/ICameraClientInstance.h"
#include "mc/deps/renderer/Camera.h"
#include "client/gameplay/ClientAim.h"
#include "client/gameplay/ClientShooter.h"
#include "client/animation/GunAnimation.h"
#include "model/FunctionalBones.h"
#include "debug/RenderDebug.h"
#include "mod/TaczMod.h"

namespace tacz::client::render {
namespace {
using Setup = void (LevelRendererPlayer::*)(mce::Camera&,float);
using Prep = void (InGamePlayScreen::*)(ScreenContext&,LevelRenderer&,Actor&);
using Render = void (InGamePlayScreen::*)(ScreenContext&,FrameRenderObject const&);
using Cursor = void (HudCursorRenderer::*)(MinecraftUIRenderContext&,IClientInstance&,UIControl&,int);
static_assert(std::is_same_v<decltype(&LevelRendererPlayer::setupCamera),Setup>);
static_assert(std::is_same_v<decltype(&InGamePlayScreen::$_renderLevelPrep),Prep>);
static_assert(std::is_same_v<decltype(&InGamePlayScreen::$render),Render>);
static_assert(std::is_same_v<decltype(&HudCursorRenderer::$render),Cursor>);
using SaveOption=void (Option::*)(std::vector<std::pair<std::string,std::string>>&);
static_assert(std::is_same_v<decltype(static_cast<SaveOption>(&Option::save)),SaveOption>);
static_assert(std::is_same_v<std::underlying_type_t<OptionID>,int>);
static_assert(entt::type_hash<MinecraftCamera::CameraComponent>::value() == 0x4f6047c7u);
static_assert(entt::type_hash<MinecraftCamera::RenderCameraComponent>::value() == 0x119e772bu);
bool setupHooked = false, prepHooked = false, renderHooked = false, cursorHooked = false;
using Registry = entt::basic_registry<EntityId>;
struct Counts {
    uint64_t entry=0, rejected=0, noScope=0, noCamera=0, injected=0, restored=0, changed=0;
    float viewOrientationError=-1, peakRoll=0;
} counts;

// Render-component-before-setup approach studied in Do-A-Barrel-Roll-Bedrock
// (GPL-3.0, 15127ea4b0b8fc741954707375ef551ce25326de, src/mod/hooks/Camera.cpp).
// CameraComponent orientation is a RENDER output, not DirectLook/Input orientation.
// Keep it until the containing native prep/render returns, then restore. All native
// identities are callback-scoped; no pending registry pointer survives a frame/world.
struct RenderScope;
thread_local RenderScope* renderScope = nullptr;
struct RenderScope {
    struct Saved { uint64_t world; IClientInstance* client; Registry const* registry; EntityId entity; glm::quat before, after; };
    std::optional<Saved> saved;
    RenderScope* previous = renderScope;
    RenderScope() { if (previous) previous->restore(); renderScope = this; }
    void restore() {
        if (!saved) return;
        auto s=*saved;
        saved.reset();
        if (shooter::worldToken() != s.world) return; // level/client may already be gone
        auto* player = s.client->getLocalPlayer();
        if (!player) return;
        auto& registry = player->getEntityContext().getRegistry();
        if (&registry != s.registry || !registry.valid(s.entity)) return;
        if (auto* cc = const_cast<MinecraftCamera::CameraComponent*>(std::as_const(registry).try_get<MinecraftCamera::CameraComponent>(s.entity))) {
            auto current = cc->mOrientation.get();
            if (current == s.after || current == -s.after) { cc->mOrientation.get() = s.before; ++counts.restored; }
            else ++counts.changed; // never overwrite a subsequent writer/mod's value
        }
    }
    ~RenderScope() { restore(); renderScope = previous; }
};
void report(uint64_t world, bool bob, bool shake, glm::vec3 angles) {
    using Clock=std::chrono::steady_clock;
    static uint64_t lastWorld=0;
    static int lastRevision=-1;
    static auto at=Clock::now();
    auto now=Clock::now();
    if (world==lastWorld && lastRevision==debug::renderRevision && now-at<std::chrono::seconds(1)) return;
    lastWorld=world; lastRevision=debug::renderRevision; at=now;
    TaczMod::getInstance().getSelf().getLogger().info(
        "[camera-bone] world={} entry={} rejected={} noScope={} noCamera={} injected={} restored={} changedBeforeRestore={} bobView={} cameraShake={} anglesDeg=({:.4f},{:.4f},{:.4f}) peakRollDeg={:.4f} nativeViewOrientationError={:.9g} source=render-camera-component playerLookWrite=false",
        world,counts.entry,counts.rejected,counts.noScope,counts.noCamera,counts.injected,counts.restored,counts.changed,
        bob,shake,angles.x,angles.y,angles.z,counts.peakRoll,counts.viewOrientationError);
    counts.peakRoll=0;
}
LL_TYPE_INSTANCE_HOOK(CameraBonePrep,ll::memory::HookPriority::Normal,InGamePlayScreen,
    &InGamePlayScreen::$_renderLevelPrep,void,ScreenContext& screen,LevelRenderer& renderer,Actor& actor) {
    RenderScope scope;
    origin(screen,renderer,actor);
}
LL_TYPE_INSTANCE_HOOK(CameraBoneRender,ll::memory::HookPriority::Normal,InGamePlayScreen,
    &InGamePlayScreen::$render,void,ScreenContext& screen,FrameRenderObject const& frame) {
    RenderScope scope;
    origin(screen,frame);
}
LL_TYPE_INSTANCE_HOOK(CameraBoneSetup,ll::memory::HookPriority::Normal,LevelRendererPlayer,
    &LevelRendererPlayer::setupCamera,void,mce::Camera& camera,float alpha) {
    if (renderScope) renderScope->restore(); // also before a rejected second setup
    auto world=shooter::worldToken();
    if (!world) { origin(camera,alpha); return; }
    ++counts.entry;
    auto pose = anim::snapshot();
    bool shake=cameraBonesEnabled(mClientInstance);
    if (!shake || pose.world != world || !pose.gun || !pose.cameraAllowed
        || pose.client != reinterpret_cast<uintptr_t>(&mClientInstance) || mClientInstance.getOptions().getPlayerViewPerspective() != 0
        || !mClientInstance.getScreenName().starts_with("hud_screen")) {
        ++counts.rejected;
        counts.viewOrientationError=-1;
        report(world,mClientInstance.getOptions().getBobView(),shake,{});
        origin(camera,alpha); return;
    }
    auto it = pose.pose.find("camera");
    glm::quat animation{1,0,0,0};
    std::optional<glm::quat> injected;
    if (it != pose.pose.end() && it->second.rotation)
        animation=model::scaledCameraRotation(model::cameraBoneRotation(*it->second.rotation),pose.cameraAim,pose.cameraZoom);
    if (animation.x==0 && animation.y==0 && animation.z==0) {
        counts.viewOrientationError=-1;
        origin(camera,alpha);
        report(world,mClientInstance.getOptions().getBobView(),shake,{});
        return; // no Euler round-trip or component write for an unanimated camera
    }
    if (!renderScope || !prepHooked || !renderHooked) {
        ++counts.noScope;
    } else if (auto* player=mClientInstance.getLocalPlayer()) {
        auto& reg=player->getEntityContext().getRegistry();
        auto const& registry=std::as_const(reg);
        auto const* tags=registry.storage<MinecraftCamera::RenderCameraComponent>();
        std::optional<EntityId> selected;
        bool ambiguous=false;
        if (tags) for (auto entity:*tags) {
            if (!registry.valid(entity) || !registry.all_of<MinecraftCamera::CameraComponent>(entity)) continue;
            if (auto const* owner=registry.try_get<CameraClientInstanceComponent>(entity);
                owner && owner->mClientInstance && owner->mClientInstance->getLocalPlayer()!=player) continue;
            if (selected) { ambiguous=true; break; }
            selected=entity;
        }
        if (selected && !ambiguous) {
            // Const registry lookup must not instantiate native component storage/constructors.
            auto& cc=const_cast<MinecraftCamera::CameraComponent&>(registry.get<MinecraftCamera::CameraComponent>(*selected));
            auto before=cc.mOrientation.get();
            auto after=model::cameraEntityOrientation(before,animation);
            if (std::isfinite(after.w) && std::isfinite(after.x) && std::isfinite(after.y) && std::isfinite(after.z)) {
                renderScope->saved=RenderScope::Saved{world,&mClientInstance,&reg,*selected,before,after};
                cc.mOrientation.get()=after;
                injected=after;
                ++counts.injected;
            } else ++counts.noCamera;
        } else ++counts.noCamera;
    }
    origin(camera,alpha); // native builds all matrices/snapshots from the injected orientation
    auto angles=glm::degrees(model::quaternionAngles(animation));
    counts.peakRoll=std::max(counts.peakRoll,std::abs(angles.z));
    counts.viewOrientationError=-1;
    if (injected && !camera.viewMatrixStack->stack->empty()) {
        auto expected=glm::transpose(glm::mat3_cast(*injected));
        auto actual=glm::mat3(camera.viewMatrixStack->top()._m.get());
        counts.viewOrientationError=0;
        for (int c=0;c<3;++c) for (int r=0;r<3;++r)
            counts.viewOrientationError=std::max(counts.viewOrientationError,std::abs(actual[c][r]-expected[c][r]));
    }
    report(world,mClientInstance.getOptions().getBobView(),shake,angles);
}
LL_TYPE_INSTANCE_HOOK(GunCrosshair,ll::memory::HookPriority::Normal,HudCursorRenderer,
    &HudCursorRenderer::$render,void,MinecraftUIRenderContext& context,IClientInstance& client,UIControl& owner,int pass) {
    if (shooter::worldToken()) {
        auto state = aim::snapshot();
        auto pose = anim::snapshot();
        if (state.world && state.gun && state.client == reinterpret_cast<uintptr_t>(&client)
            && pose.world == state.world && pose.gun == state.gun) {
            auto const& packs = TaczMod::getInstance().gunPacks();
            auto const* gun = packs.gun(*state.gun);
            auto const* display = gun && gun->index.display ? packs.gunDisplay(*gun->index.display) : nullptr;
            if (gun::isReloading(shooter::reloadState()) || pose.hideCrosshair
                || (state.progress > .9f && display && !display->showCrosshair)) return;
        }
    }
    origin(context,client,owner,pass);
}
}
bool cameraBonesEnabled(IClientInstance& client) {
    if (!shooter::worldToken()) return false; // no option/client/world lookup on the main menu
    // SDK OptionID is an empty enum. 1.26.51.01's video.camera_shake and
    // accessibility.camera_shake both bind 0x23A; validate native save tag too.
    auto option=client.getOptions().getIfValid(static_cast<OptionID>(0x23a));
    int state=-1;
    if (option && *option && (*option)->mImpl && (*option)->mImpl->mSaveTag.get()=="camera_shake") {
        // Public virtual serialization into a local vector, NOT disk I/O. No
        // BoolOption downcast, opaque iterator, guessed enum type or retained pointer.
        std::vector<std::pair<std::string,std::string>> values;
        (*option)->save(values);
        for (auto const& [key,value]:values) if (key=="camera_shake") {
            if (value=="1" || value=="true") state=1;
            else if (value=="0" || value=="false") state=0;
        }
    }
    static std::atomic<int> last{-2};
    if (last.exchange(state)!=state) TaczMod::getInstance().getSelf().getLogger().info(
        "[camera-option] id=570 saveTag=camera_shake value={} (-1=unavailable: disabled) live=true",state);
    return state==1;
}
void enableCameraBones() {
    counts={};
    prepHooked = CameraBonePrep::hook() == 0;
    renderHooked = CameraBoneRender::hook() == 0;
    setupHooked = CameraBoneSetup::hook() == 0;
    cursorHooked = GunCrosshair::hook() == 0;
    TaczMod::getInstance().getSelf().getLogger().info("[camera-bone] setup/prep/render/crosshair hooks={}/{}/{}/{}",setupHooked,prepHooked,renderHooked,cursorHooked);
}
void disableCameraBones() {
    if (setupHooked) CameraBoneSetup::unhook();
    if (prepHooked) CameraBonePrep::unhook();
    if (renderHooked) CameraBoneRender::unhook();
    if (cursorHooked) GunCrosshair::unhook();
    setupHooked = prepHooked = renderHooked = cursorHooked = false;
}
} // namespace tacz::client::render
