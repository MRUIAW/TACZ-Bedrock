#include "client/animation/ViewTurnInertia.h"

#include <mutex>
#include <cmath>
#include <utility>
#include "ll/api/service/TargetedBedrock.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/ecs/WeakEntityRef.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/entity/components/ActorHeadRotationComponent.h"
#include "client/gameplay/ClientShooter.h"

namespace tacz::client::turn {
namespace {
std::mutex mutex;
struct State {
    uint64_t world=0;
    uintptr_t client=0, player=0;
    bool alive=false;
    model::ViewTurnBob bob;
} state;
bool matches(State const& s, uint64_t world, IClientInstance& client, LocalPlayer& player) {
    return world && s.world==world && s.client==reinterpret_cast<uintptr_t>(&client)
        && s.player==reinterpret_cast<uintptr_t>(&player);
}
}
void clear() { std::lock_guard lock(mutex); state={}; }
void tick() {
    auto world=shooter::worldToken();
    if (!world) { clear(); return; }
    auto client=ll::service::getClientInstance();
    if (!client || !shooter::ownsClient(&*client)) { clear(); return; }
    auto* player=client->getLocalPlayer();
    if (!player) { clear(); return; }
    auto rotation=player->getRotation(); // actor pitch .x / yaw .z, never camera matrices
    bool alive=player->isAlive() && !player->isDead();
    auto camera=client->getCameraEntity().lock();
    auto const& entity=player->getEntityContext();
    bool controlled=camera && &camera->getRegistry()==&entity.getRegistry() && camera->mEntity==entity.mEntity;
    std::lock_guard lock(mutex); // publish a coherent value snapshot to the render thread
    if (!matches(state,world,*client,*player) || (!state.alive && alive)) {
        state={world,reinterpret_cast<uintptr_t>(&*client),reinterpret_cast<uintptr_t>(player),alive,{}};
        state.bob.initialize(rotation.x,rotation.z);
    } else state.bob.tick(rotation.x,rotation.z,alive && !player->isSleeping() && controlled);
    state.alive=alive;
}
Sample sample(IClientInstance& client, LocalPlayer& player, float alpha) {
    State copy;
    { std::lock_guard lock(mutex); copy=state; }
    auto world=shooter::worldToken();
    if (!matches(copy,world,client,player) || !shooter::ownsClient(&client)) return {};
    if (!copy.alive && player.isAlive() && !player.isDead()) return {}; // await respawn initialization
    auto rotation=player.getRotation();
    double yaw=rotation.z;
    if (player.getVehicle()) {
        // Java ticks bob from BODY yaw, but getViewYRot(alpha) uses interpolated
        // HEAD yaw while riding. Const typed lookup never creates component storage.
        auto head=std::as_const(player.getEntityContext()).tryGetComponent<ActorHeadRotationComponent>();
        if (head) yaw=std::lerp(static_cast<double>(head->mYHeadRotO),static_cast<double>(head->mYHeadRot),
            static_cast<double>(alpha));
    }
    return {copy.bob.frame(rotation.x,yaw,alpha),copy.bob.pitch,copy.bob.yaw};
}
} // namespace tacz::client::turn
