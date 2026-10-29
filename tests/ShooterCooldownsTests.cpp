#include <doctest/doctest.h>

#include "gun/ShooterCooldowns.h"
#include "gun/SyncedShooterData.h"

using namespace tacz;

TEST_CASE("Synced shoot cooldown getter distinguishes no gun unknown gun and no accepted shot") {
    pack::GunData data; data.rpm = 600;
    CHECK(gun::syncedShootCooldownMs(false, nullptr, {}, 1000, {}, {}) == 0);
    CHECK(gun::syncedShootCooldownMs(false, &data, {}, 1000, {}, {}) == 0);
    CHECK(gun::syncedShootCooldownMs(true, nullptr, {}, 1000, {}, {}) == -1);
    CHECK(gun::syncedShootCooldownMs(true, &data, {}, 1000, {}, {}) == 0);
    CHECK(gun::syncedShootCooldownMs(true, &data, pack::FireMode::Auto, 1000, 1234, {}) == 0);
}

TEST_CASE("Synced shoot cooldown getter uses Java normal burst minimum and the 5 ms boundary") {
    pack::GunData data; data.rpm = 600; data.burst.minInterval = .57f; data.burst.bpm = 1200;
    for (auto mode : {pack::FireMode::Auto, pack::FireMode::Semi, pack::FireMode::Burst}) {
        int64_t duration = mode == pack::FireMode::Burst ? 570 : 100;
        CHECK(gun::syncedShootCooldownMs(true, &data, mode, 5000, 4000, 1000) == duration - 5);
        CHECK(gun::syncedShootCooldownMs(true, &data, mode, 5000 + duration - 6, 4000, 1000) == 1);
        CHECK(gun::syncedShootCooldownMs(true, &data, mode, 5000 + duration - 5, 4000, 1000) == 0);
        CHECK(gun::syncedShootCooldownMs(true, &data, mode, 9000, 4000, 1000) == 0);
    }
    CHECK(gun::triggerIntervalMs(data, pack::FireMode::Burst) == 570); // float multiply, not double truncation to 569
    CHECK(gun::burstShootIntervalMs(data) == 50); // NOT the burst trigger minimum
}

TEST_CASE("Synced shoot cooldown uses translated client timeline rather than receipt time under jitter") {
    pack::GunData data; data.rpm = 600;
    // Offset established earlier. A shot timestamp 1000 arrives late at server 5070.
    CHECK(gun::syncedShootCooldownMs(true, &data, pack::FireMode::Auto, 5080, 4000, 1000) == 15);
    CHECK(gun::serverCooldownMs(data, pack::FireMode::Auto, 5080, 5070) == 85); // old receipt-clock result
    CHECK(gun::syncedShootCooldownMs(true, &data, pack::FireMode::Auto, 5095, 4000, 1000) == 0);
    // A future accepted client stamp can produce more than one interval, like Java.
    CHECK(gun::syncedShootCooldownMs(true, &data, pack::FireMode::Auto, 5050, 4000, 1100) == 145);
}

TEST_CASE("Synced draw cooldown getter distinguishes no gun unknown and Java future timestamps") {
    pack::GunData data; data.drawTime = .2f;
    gun::DrawCooldown draw;
    CHECK(gun::syncedDrawCooldownMs(false, nullptr, draw, 0) == 0);
    CHECK(gun::syncedDrawCooldownMs(true, nullptr, draw, 0) == -1);
    CHECK(gun::syncedDrawCooldownMs(true, &data, draw, 0) == 0);
    draw.select(1000, 300);
    CHECK(gun::syncedDrawCooldownMs(true, &data, draw, 1000) == 195);
    CHECK(gun::syncedDrawCooldownMs(true, &data, draw, 1194) == 1);
    CHECK(gun::syncedDrawCooldownMs(true, &data, draw, 1195) == 0);
    draw.select(2000, 300); // old put-away, timestamp is now in the future
    REQUIRE(draw.drawTimestamp == 2300);
    CHECK(gun::syncedDrawCooldownMs(true, &data, draw, 2000) == 495);
    draw.select(2050, 50); // during put-away preserves the timestamp, not now+put-away
    CHECK(draw.drawTimestamp == 2300);
    CHECK(gun::syncedDrawCooldownMs(true, &data, draw, 2050) == 445);
}

TEST_CASE("Synced draw cooldown handles interrupted draw and zero draw time without hiding put away") {
    pack::GunData data; data.drawTime = 0;
    gun::DrawCooldown draw;
    draw.select(1000, 300);
    CHECK(gun::syncedDrawCooldownMs(true, &data, draw, 1000) == 0);
    draw.select(1050, 300); // elapsed draw is only 50 ms, so carry 50 not 300
    CHECK(draw.drawTimestamp == 1100);
    CHECK(gun::syncedDrawCooldownMs(true, &data, draw, 1050) == 45);
    CHECK(gun::syncedDrawCooldownMs(true, &data, draw, 1095) == 0);
    draw.select(1060, 0);
    CHECK(draw.drawTimestamp == 1100);
    CHECK(gun::syncedDrawCooldownMs(true, &data, draw, 1060) == 35);
}

TEST_CASE("Synced melee cooldown getter with timestamp unset is zero only for known or absent guns") {
    pack::GunData data;
    CHECK(gun::syncedMeleeCooldownMs(false, nullptr) == 0);
    CHECK(gun::syncedMeleeCooldownMs(false, &data) == 0);
    CHECK(gun::syncedMeleeCooldownMs(true, nullptr) == -1);
    CHECK(gun::syncedMeleeCooldownMs(true, &data) == 0);
}

TEST_CASE("Eight produced shooter keys fit revision one lifecycle snapshots and idle emits nothing after publication") {
    namespace sync = gun::sync;
    sync::Snapshot state;
    state.set({sync::Key::ShootCoolDown, gun::syncedShootCooldownMs(false, nullptr, {}, 1000, {}, {})});
    state.set({sync::Key::DrawCoolDown, gun::syncedDrawCooldownMs(false, nullptr, {}, 1000)});
    state.set({sync::Key::MeleeCoolDown, gun::syncedMeleeCooldownMs(false, nullptr)});
    auto full = sync::full(0x100000001ull, state);
    REQUIRE(full.entries.size() == 8);
    CHECK(full.entries.back().key == sync::Key::IsBolting);
    auto decoded = sync::decode(sync::encode(full)); REQUIRE(decoded);
    CHECK(sync::ProtocolRevision == 1);
    CHECK(decoded.message.entries.size() == 8);
    sync::Baseline baseline;
    CHECK(baseline.changed(1, state).entries.size() == 3); // missing (-1) -> initialized idle (0)
    baseline.commit(state);
    for (int tick = 0; tick < 100; ++tick) CHECK(baseline.changed(1, state).entries.empty());
    CHECK(sync::full(1, state).entries.size() == 8);
    CHECK(baseline.changed(1, state).entries.empty());
}
