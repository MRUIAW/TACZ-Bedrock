#pragma once
#include <optional>
#include "model/ViewTurnInertia.h"
class ScreenContext;
class IClientInstance;
class Matrix;
namespace tacz::client::render {
// Callback-scoped TLS; nested screen callbacks retire their parent's capture too.
class HandTurnScope {
    HandTurnScope* previous;
public:
    uint64_t generation;
    ScreenContext& screen;
    struct Capture { model::HandTurnCaptureKey key; glm::mat4 base; };
    std::optional<Capture> capture;
    explicit HandTurnScope(ScreenContext& screen);
    ~HandTurnScope();
    HandTurnScope(HandTurnScope const&)=delete;
    HandTurnScope& operator=(HandTurnScope const&)=delete;
};
struct HandTurnBase {
    std::optional<glm::mat4> captured;
};
void captureHandTurn(IClientInstance& client, Matrix const& mv);
HandTurnBase consumeHandTurn(IClientInstance& client, ScreenContext& screen);
} // namespace tacz::client::render
