#include "client/ClientProbe.h"

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/ListenerBase.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/TargetedBedrock.h"
#include "ll/api/thread/ClientThreadExecutor.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/client/game/IMinecraftGame.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/Font.h"
#include "mc/client/gui/FontHandle.h"
#include "mc/client/gui/FontRepository.h"
#include "mc/client/gui/TextAlignment.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/client/gui/controls/VisualTree.h"
#include "mc/client/gui/screens/ScreenView.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/input/HIDController.h"
#include "mc/deps/input/MouseAction.h"
#include "mc/deps/input/RectangleArea.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/world/item/ItemStack.h"

#include "nbt/GunNbt.h"
#include "net/DebugPackets.h"

namespace tacz::client::probe {
namespace {

std::mutex  replyMutex;
std::string lastReply = "no reply yet";
std::atomic<unsigned int> nextSeq{1};

// Server -> client reply. Handlers may run off the render thread.
class AckHandler final : public ll::network::PacketHandlerBase<AckHandler, net::DebugAckPacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, net::DebugAckPacket const& packet) const {
        std::lock_guard lock(replyMutex);
        lastReply = packet.info;
    }
};

// Held-gun state as the client sees it after the server's inventory sync.
struct HeldGun {
    std::string id;
    int         ammo = -1;
};

HeldGun heldGun() {
    HeldGun gun;
    auto client = ll::service::getClientInstance();
    if (!client) return gun;
    auto* player = client->getLocalPlayer();
    if (!player) return gun;
    auto const& tag = player->getSelectedItem().mUserData;
    if (!tag || !tag->contains(nbt::GunId, Tag::String)) return gun;
    gun.id = static_cast<std::string const&>((*tag)[nbt::GunId]);
    if (tag->contains(nbt::GunCurrentAmmoCount, Tag::Int)) {
        gun.ammo = static_cast<int>((*tag)[nbt::GunCurrentAmmoCount]);
    }
    return gun;
}

bool inGameplay() {
    auto client = ll::service::getClientInstance();
    return client && client->getLocalPlayer() && client->getScreenName().starts_with("hud_screen");
}

// Input callbacks come from the window procedure; touch game state on the client thread only.
void sendAction(std::string action) {
    ll::thread::ClientThreadExecutor::getDefault().execute([action = std::move(action)] {
        net::DebugActionPacket{nextSeq++, action}.sendToServer();
    });
}

bool hudView(ScreenView const& view) {
    auto const& tree = view.mVisualTree;
    return tree && std::string_view(*tree->mRootControlName).ends_with(".hud_screen");
}

void drawLine(MinecraftUIRenderContext& context, Font& font, float x, float y, std::string text) {
    TextMeasureData const  measure{1.0f, 0.0f, true, false, false, ui::TextAlignment::Left};
    CaretMeasureData const caret{-1, false};
    context.drawText(
        font,
        RectangleArea{x, x + 400, y, y + 10},
        std::move(text),
        mce::Color{1.f, 1.f, 0.4f, 1.f},
        1.0f,
        ui::TextAlignment::Left,
        measure,
        caret
    );
}

std::array<ll::event::ListenerPtr, 3> listeners;

} // namespace

void enable() {
    auto& bus = ll::event::EventBus::getInstance();

    // R: "reload" ping; works with or without a gun, so it tests the packet path alone.
    listeners[0] = bus.emplaceListener<ll::event::input::KeyInputEvent>([](ll::event::input::KeyInputEvent& event) {
        if (!event.isDown() || event.controller().mTextboxIsFocused || !inGameplay()) return;
        if (event.keyCode() == 'R') sendAction("reload");
    });

    // Left mouse while holding a gun: swallow vanilla attack and send "fire" instead.
    listeners[1] = bus.emplaceListener<ll::event::input::MouseInputEvent>([](ll::event::input::MouseInputEvent& event) {
        if (event.actionButtonId() != MouseAction::ActionLeft || !inGameplay()) return;
        if (heldGun().id.empty()) return;
        event.cancel();
        if (event.buttonData() == MouseAction::DataDown) sendAction("fire");
    });

    listeners[2] = bus.emplaceListener<ll::event::AfterUIRenderEvent>([](ll::event::AfterUIRenderEvent& event) {
        if (!hudView(event.screenView())) return;
        auto& context = event.uiRenderContext();
        auto& font =
            context.mClient.getMinecraftGame_DEPRECATED().getFontRepository()->getFontFromFontType("default").getFont();
        auto gun = heldGun();
        std::string reply;
        {
            std::lock_guard lock(replyMutex);
            reply = lastReply;
        }
        drawLine(context, font, 4, 4, "[TACZ probe] held: " + (gun.id.empty() ? std::string{"-"} : gun.id + " ammo=" + std::to_string(gun.ammo)));
        drawLine(context, font, 4, 14, "[TACZ probe] server: " + reply);
        context.flushText(0, std::nullopt);
    });
}

void disable() {
    auto& bus = ll::event::EventBus::getInstance();
    for (auto& listener : listeners) {
        if (listener) bus.removeListener(listener);
        listener = nullptr;
    }
}

} // namespace tacz::client::probe
