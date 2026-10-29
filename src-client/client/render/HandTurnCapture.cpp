#include "client/render/HandTurnCapture.h"

#include "mc/client/game/IClientInstance.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "client/gameplay/ClientShooter.h"

namespace tacz::client::render {
namespace {
thread_local HandTurnScope* scope=nullptr;
thread_local uint64_t serial=0;
model::HandTurnCaptureKey keyFor(IClientInstance& client, ScreenContext& screen) {
    auto world=shooter::worldToken();
    if (!scope || &scope->screen!=&screen || !world || !shooter::ownsClient(&client)) return {};
    auto* player=client.getLocalPlayer();
    auto& camera=screen.camera;
    auto& stack=camera.worldMatrixStack.get();
    if (!player || stack.stack->empty()) return {};
    return {scope->generation,world,reinterpret_cast<uintptr_t>(&client),reinterpret_cast<uintptr_t>(player),
        reinterpret_cast<uintptr_t>(&camera),reinterpret_cast<uintptr_t>(&stack),
        reinterpret_cast<uintptr_t>(&stack.stack->back()),stack.stack->size()};
}
}
HandTurnScope::HandTurnScope(ScreenContext& screen) : previous(scope), generation(++serial), screen(screen) {
    if (previous) previous->capture.reset();
    scope=this;
}
HandTurnScope::~HandTurnScope() { capture.reset(); scope=previous; }
void captureHandTurn(IClientInstance& client, Matrix const& mv) {
    if (!scope) return;
    auto key=keyFor(client,scope->screen);
    if (!key.ready() || key.top!=reinterpret_cast<uintptr_t>(&mv)) return;
    scope->capture=HandTurnScope::Capture{key,mv._m.get()}; // value copy, never a retained native matrix pointer
}
HandTurnBase consumeHandTurn(IClientInstance& client, ScreenContext& screen) {
    if (!scope || !scope->capture) return {};
    auto capture=*scope->capture;
    scope->capture.reset(); // single use; validation is against the PRE-push top at hook entry
    if (!model::validHandTurnCapture(capture.key,keyFor(client,screen))) return {};
    return {capture.base};
}
} // namespace tacz::client::render
