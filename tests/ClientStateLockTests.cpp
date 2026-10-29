#include <doctest/doctest.h>

#include "gun/ClientStateLock.h"
#include "gun/ShootRules.h"

using namespace tacz;
namespace {
using gun::LockCondition;
namespace sync = gun::sync;
constexpr std::array conditions{LockCondition::Shoot, LockCondition::Draw, LockCondition::Reload, LockCondition::Bolt, LockCondition::Melee};
sync::Snapshot idle() {
    sync::Snapshot state;
    for (auto key : {sync::Key::ShootCoolDown, sync::Key::DrawCoolDown, sync::Key::MeleeCoolDown}) state.set({key, int64_t{0}});
    return state;
}
sync::Snapshot active(LockCondition condition) {
    auto state = idle();
    switch (condition) {
    case LockCondition::Shoot: state.set({sync::Key::ShootCoolDown, int64_t{10}}); break;
    case LockCondition::Draw: state.set({sync::Key::DrawCoolDown, int64_t{10}}); break;
    case LockCondition::Reload: state.set({sync::Key::ReloadState, gun::ReloadState{gun::ReloadStateType::EmptyFeeding, 10}}); break;
    case LockCondition::Bolt: state.set({sync::Key::IsBolting, true}); break;
    case LockCondition::Melee: state.set({sync::Key::MeleeCoolDown, int64_t{10}}); break;
    case LockCondition::None: break;
    }
    return state;
}
}

TEST_CASE("Java state lock holds every unmet predicate before 250 ms and times it out at exactly 250") {
    for (auto condition : conditions) {
        gun::ClientStateLock lock;
        lock.lockState(condition, 1000);
        CHECK(lock.clientStateLock);
        CHECK(lock.lockTimestamp == 1000);
        for (int64_t now : {1000, 1001, 1249}) {
            CHECK_FALSE(lock.tickStateLock(now, idle()));
            CHECK(lock.clientStateLock);
            CHECK(lock.lockedCondition == condition);
        }
        CHECK(lock.tickStateLock(1250, idle()));
        CHECK_FALSE(lock.clientStateLock);
        CHECK(lock.lockedCondition == LockCondition::None);
        CHECK_FALSE(lock.tickStateLock(1300, idle())); // count timeout once, not every tick
    }
}

TEST_CASE("Java state lock sees each predicate before 250 and then waits for authoritative completion") {
    for (auto condition : conditions) {
        gun::ClientStateLock lock;
        lock.lockState(condition, 1000);
        CHECK_FALSE(lock.tickStateLock(1040, active(condition)));
        CHECK(lock.clientStateLock);
        CHECK(lock.lockedCondition == LockCondition::None);
        CHECK_FALSE(lock.tickStateLock(1400, active(condition))); // no timeout overrides positive authority
        CHECK(lock.clientStateLock);
        CHECK_FALSE(lock.tickStateLock(1450, idle()));
        CHECK_FALSE(lock.clientStateLock);
    }
}

TEST_CASE("Java state lock timeout clears the predicate but cannot override any other authoritative action") {
    for (auto waiting : conditions) for (auto running : conditions) {
        if (waiting == running) continue;
        gun::ClientStateLock lock;
        lock.lockState(waiting, 1000);
        CHECK(lock.tickStateLock(1250, active(running)));
        CHECK(lock.clientStateLock);
        CHECK(lock.lockedCondition == LockCondition::None);
        CHECK_FALSE(lock.tickStateLock(5000, active(running)));
        CHECK(lock.clientStateLock);
        CHECK_FALSE(lock.tickStateLock(5050, idle()));
        CHECK_FALSE(lock.clientStateLock);
    }
}

TEST_CASE("Java state lock predicate seen at timeout is satisfied not a diagnostic timeout") {
    for (auto condition : conditions) {
        gun::ClientStateLock lock;
        lock.lockState(condition, 1000);
        CHECK_FALSE(lock.tickStateLock(1250, active(condition)));
        CHECK(lock.clientStateLock);
        CHECK(lock.lockedCondition == LockCondition::None);
    }
}

TEST_CASE("Java state lock release uses positive cooldowns while shoot precheck rejects nonzero missing values") {
    for (int64_t value : {-1, 0, 1}) {
        for (auto key : {sync::Key::ShootCoolDown, sync::Key::DrawCoolDown, sync::Key::MeleeCoolDown}) {
            auto state = idle(); state.set({key, value});
            gun::ClientStateLock lock;
            lock.lockState(LockCondition::None, 1000);
            CHECK_FALSE(lock.tickStateLock(1001, state));
            CHECK(lock.clientStateLock == (value > 0));
            CHECK(gun::syncedShootPrecheck(state) == (key == sync::Key::ShootCoolDown || value == 0));
        }
    }
    CHECK(gun::syncedShootPrecheck(idle()));
    CHECK_FALSE(gun::syncedShootPrecheck(active(LockCondition::Reload)));
}

TEST_CASE("Java Shoot lock permits the next locally rate eligible shot while pending non Shoot conditions reject it") {
    pack::GunData data; data.rpm = 600;
    for (auto condition : conditions) {
        gun::ClientStateLock lock; lock.lockState(condition, 1000);
        CHECK(gun::clientCooldownMs(data, pack::FireMode::Auto, 1100, 1000) == 0);
        CHECK(lock.pendingNonShoot() == (condition != LockCondition::Shoot));
        // General clientStateLock remains true even if a Shoot action permits firing.
        CHECK(lock.clientStateLock);
    }
    gun::ClientStateLock lock;
    lock.lockState(LockCondition::Draw, 1000);
    lock.tickStateLock(1050, active(LockCondition::Draw));
    CHECK_FALSE(lock.pendingNonShoot()); // predicate acknowledged, authority now gates firing
    CHECK_FALSE(gun::syncedShootPrecheck(active(LockCondition::Draw)));
}

TEST_CASE("Java unobserved short operation times out and repeated successful shots replace the timestamp") {
    gun::ClientStateLock lock;
    lock.lockState(LockCondition::Shoot, 1000);
    lock.tickStateLock(1099, idle());
    lock.lockState(LockCondition::Shoot, 1100);
    CHECK(lock.lockTimestamp == 1100);
    CHECK_FALSE(lock.tickStateLock(1250, idle()));
    CHECK(lock.clientStateLock);
    CHECK(lock.tickStateLock(1350, idle()));
    CHECK_FALSE(lock.clientStateLock);
}

TEST_CASE("Java tick never acquires a lock even when authoritative actions are active") {
    for (auto condition : conditions) {
        gun::ClientStateLock lock;
        CHECK_FALSE(lock.tickStateLock(1000, active(condition)));
        CHECK_FALSE(lock.clientStateLock);
        CHECK(lock.lockedCondition == LockCondition::None);
        CHECK_FALSE(lock.tickStateLock(1010, idle()));
        CHECK_FALSE(lock.clientStateLock);
    }
}

TEST_CASE("Java inspect and fire select only read the lock and reset clears it") {
    gun::ClientStateLock lock;
    // Both permission checks only read clientStateLock, never lockState.
    for (int i = 0; i < 100; ++i) CHECK_FALSE(lock.clientStateLock);
    CHECK(lock.lockTimestamp == -1);
    lock.lockState(LockCondition::Reload, 1234);
    CHECK(lock.clientStateLock);
    CHECK(lock.clientStateLock);
    CHECK(lock.lockTimestamp == 1234);
    lock.reset();
    CHECK_FALSE(lock.clientStateLock);
    CHECK(lock.lockedCondition == LockCondition::None);
    CHECK(lock.lockTimestamp == -1);
    CHECK_FALSE(lock.pendingNonShoot());
}

TEST_CASE("Java state lock has no local decay and accepts future Bolt and Melee conditions") {
    for (auto condition : conditions) {
        auto state = active(condition);
        gun::ClientStateLock lock;
        lock.lockState(condition, 0);
        for (int64_t tick = 0; tick <= 5000; tick += 50) {
            CHECK_FALSE(lock.tickStateLock(tick, state));
            CHECK(lock.clientStateLock);
        }
        CHECK_FALSE(lock.tickStateLock(5050, idle()));
        CHECK_FALSE(lock.clientStateLock);
    }
}

TEST_CASE("Draw acquisition replaces pending prediction without modifying the synced entity view") {
    sync::EntityStore store; store.begin(1);
    store.apply(1, {9, {{sync::Key::ReloadState, gun::ReloadState{gun::ReloadStateType::EmptyFeeding, 123}}}});
    gun::ClientStateLock lock;
    lock.lockState(LockCondition::Shoot, 1000);
    lock.lockState(LockCondition::Draw, 1010);
    CHECK(lock.pendingNonShoot());
    CHECK(lock.lockTimestamp == 1010);
    CHECK(store.local(1, 9).reload().countDown == 123);
    CHECK(lock.lockedCondition == LockCondition::Draw);
}
