#include "client/render/CameraBones.h"

#include <type_traits>
#include <optional>
#include "ll/api/memory/Hook.h"
#include "ll/api/io/Logger.h"
#include "mc/client/renderer/game/LevelRendererPlayer.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/client/gui/controls/renderers/HudCursorRenderer.h"
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
using Dependencies = void (mce::Camera::*)();
using Cursor = void (HudCursorRenderer::*)(MinecraftUIRenderContext&,IClientInstance&,UIControl&,int);
static_assert(std::is_same_v<decltype(&LevelRendererPlayer::setupCamera),Setup>);
static_assert(std::is_same_v<decltype(&mce::Camera::updateViewMatrixDependencies),Dependencies>);
static_assert(std::is_same_v<decltype(&HudCursorRenderer::$render),Cursor>);
struct SetupScope {
    mce::Camera* camera;
    glm::quat rotation;
    bool consumed = false;
    std::optional<glm::mat4> appliedView;
};
thread_local SetupScope* setup = nullptr;
bool setupHooked = false, dependenciesHooked = false, cursorHooked = false;
uint64_t loggedWorld = 0;
unsigned samples = 0;
int diagnosticRevision = -1;

LL_TYPE_INSTANCE_HOOK(CameraBoneDependencies,ll::memory::HookPriority::Normal,mce::Camera,
    &mce::Camera::updateViewMatrixDependencies,void) {
    // Inject BEFORE the native dependency calculation and its callers' snapshots,
    // not after setupCamera returns (Lamium found that too late for detached render).
    if (setup && setup->camera == this && !setup->consumed && !viewMatrixStack->stack->empty()) {
        viewMatrixStack->getTop()._m = model::applyCameraAngles(viewMatrixStack->top()._m.get(),setup->rotation);
        setup->appliedView = viewMatrixStack->top()._m.get();
        setup->consumed = true;
    }
    origin(); // inverse view, camera axes and frustum all follow the changed view
}
LL_TYPE_INSTANCE_HOOK(CameraBoneSetup,ll::memory::HookPriority::Normal,LevelRendererPlayer,
    &LevelRendererPlayer::setupCamera,void,mce::Camera& camera,float alpha) {
    if (!dependenciesHooked || !shooter::worldToken()) { origin(camera,alpha); return; }
    auto pose = anim::snapshot();
    if (pose.world != shooter::worldToken() || !pose.gun || !pose.cameraAllowed
        || pose.client != reinterpret_cast<uintptr_t>(&mClientInstance) || mClientInstance.getOptions().getPlayerViewPerspective() != 0
        || !mClientInstance.getOptions().getBobView()) {
        origin(camera,alpha); return;
    }
    auto it = pose.pose.find("camera");
    if (it == pose.pose.end() || !it->second.rotation) { origin(camera,alpha); return; }
    SetupScope scope{&camera,model::scaledCameraRotation(model::cameraBoneRotation(*it->second.rotation),pose.cameraAim,pose.cameraZoom)};
    struct Restore { SetupScope* old = setup; ~Restore() { setup = old; } } restore;
    setup = &scope;
    origin(camera,alpha);
    bool retained = scope.appliedView && !camera.viewMatrixStack->stack->empty()
        && *scope.appliedView == camera.viewMatrixStack->top()._m.get();
    if (loggedWorld != pose.world || diagnosticRevision != debug::renderRevision) {
        loggedWorld = pose.world; diagnosticRevision = debug::renderRevision; samples = 0;
    }
    if (!scope.consumed && samples == 0) TaczMod::getInstance().getSelf().getLogger().warn(
        "[camera-bone] native setup did not reach the scoped view-dependency path; world shake is NOT applied; no player-look fallback");
    if (samples++ < 8) TaczMod::getInstance().getSelf().getLogger().info(
        "[camera-bone] world={} before-dependencies={} finalViewRetained={} anglesDeg=({:.4f},{:.4f},{:.4f}) playerRotationWrite=false",
        pose.world,scope.consumed,retained,glm::degrees(model::quaternionAngles(scope.rotation)).x,
        glm::degrees(model::quaternionAngles(scope.rotation)).y,glm::degrees(model::quaternionAngles(scope.rotation)).z);
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
void enableCameraBones() {
    dependenciesHooked = CameraBoneDependencies::hook() == 0;
    setupHooked = CameraBoneSetup::hook() == 0;
    cursorHooked = GunCrosshair::hook() == 0;
    TaczMod::getInstance().getSelf().getLogger().info("[camera-bone] setup/dependencies/crosshair hooks={}/{}/{}",setupHooked,dependenciesHooked,cursorHooked);
}
void disableCameraBones() {
    if (setupHooked) CameraBoneSetup::unhook();
    if (dependenciesHooked) CameraBoneDependencies::unhook();
    if (cursorHooked) GunCrosshair::unhook();
    setupHooked = dependenciesHooked = cursorHooked = false;
}
} // namespace tacz::client::render
