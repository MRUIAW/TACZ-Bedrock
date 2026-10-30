#include "client/HitDiagnostic.h"

#include <mutex>
#include "ll/api/io/Logger.h"
#include "mc/deps/core/utility/optional_ref.h"
#include "ll/api/network/packet/Packet.h"
#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/legacy/ActorRuntimeID.h"
#include "mc/world/phys/HitResult.h"
#include "client/gameplay/ClientShooter.h"
#include "debug/HitDiagnostic.h"
#include "mod/TaczMod.h"
#include "net/DebugPackets.h"

namespace tacz::client::hitdiag {
namespace {
std::mutex mutex;
debug::hit::Budget budget;
uint64_t world = 0;
uint64_t ticks = 0, armedAt = 0;
class HitDiagnosticHandler final : public ll::network::PacketHandlerBase<HitDiagnosticHandler, net::HitDiagnosticPacket> {
public:
    void handlePacket(NetworkIdentifier const&, NetEventCallback&, net::HitDiagnosticPacket const& packet) const {
        std::lock_guard lock(mutex);
        world = shooter::worldToken();
        budget.arm(world ? packet.shots : 0);
        armedAt = ticks;
    }
};
}
void clear() { std::lock_guard lock(mutex); budget.arm(0); world = 0; }
void tick() {
    std::lock_guard lock(mutex);
    ++ticks;
    if (!world) return;
    if (world != shooter::worldToken() || ticks - armedAt >= debug::hit::TickLimit) {
        budget.arm(0); world = 0;
    }
}
void shot(int64_t timestamp) {
    std::lock_guard lock(mutex);
    if (!world || world != shooter::worldToken()) return;
    auto tag = budget.trigger(0, timestamp);
    if (!tag || !budget.take(*tag)) return;
    auto client = ll::service::getClientInstance();
    if (!client || !shooter::ownsClient(&*client)) return;
    auto player = client->getLocalPlayer();
    if (!player) return;
    auto const& hit = client->getLatestHitResult();
    auto entity = hit.mType == HitResultType::Entity ? hit.getEntity() : nullptr;
    auto eye = player->getEyePos();
    auto direction = player->getViewVector();
    auto rotation = player->getRotation();
    auto picked = entity ? entity->getPosition() : Vec3{};
    TaczMod::getInstance().getSelf().getLogger().info(
        "[hit-diag-client] line={}/{} shot={} triggerTs={} world={} eye=({:.6f},{:.6f},{:.6f}) direction=({:.6f},{:.6f},{:.6f}) rotation=({:.6f},{:.6f}) pickType={} pickRuntime={} pickName={} pickPos=({:.6f},{:.6f},{:.6f}) pickHit=({:.6f},{:.6f},{:.6f}) pickRayStart=({:.6f},{:.6f},{:.6f}) pickRayDirection=({:.6f},{:.6f},{:.6f})",
        budget.used(), debug::hit::LineLimit, tag->shot, timestamp, world, eye.x, eye.y, eye.z,
        direction.x, direction.y, direction.z, rotation.x, rotation.z, static_cast<int>(hit.mType),
        entity ? entity->getRuntimeID().rawID : 0, entity ? entity->getTypeName() : "none", picked.x, picked.y, picked.z,
        hit.mPos.x, hit.mPos.y, hit.mPos.z, hit.mStartPos.x, hit.mStartPos.y, hit.mStartPos.z,
        hit.mRayDir.x, hit.mRayDir.y, hit.mRayDir.z);
}
} // namespace tacz::client::hitdiag
