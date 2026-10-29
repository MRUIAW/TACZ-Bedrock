#include <doctest/doctest.h>

#include <limits>
#include "gun/ClientWorldSession.h"
#include "gun/Sprint.h"
#include "gun/SyncedShooterData.h"

using namespace tacz;
namespace {
namespace sync = gun::sync;
constexpr std::array allKeys{sync::Key::ShootCoolDown, sync::Key::MeleeCoolDown, sync::Key::ReloadState,
    sync::Key::AimingProgress, sync::Key::DrawCoolDown, sync::Key::IsAiming, sync::Key::SprintTime, sync::Key::IsBolting};
constexpr uint64_t actorId = 0xfedcba9876543210ull;
sync::Snapshot active() {
    sync::Snapshot state;
    state.set({sync::Key::ReloadState, gun::ReloadState{gun::ReloadStateType::TacticalFeeding, 813}});
    state.set({sync::Key::SprintTime, .25f});
    return state;
}
void checkRejected(std::string const& bytes, sync::DecodeError expected) {
    auto result = sync::decode(bytes);
    CHECK_FALSE(result);
    CHECK(result.error == expected);
    CHECK(result.message.entries.empty());
    CHECK(result.message.runtimeId == 0);
}
}

TEST_CASE("Shooter sync schema follows Java declaration order types defaults and ALL") {
    sync::Snapshot state;
    REQUIRE(sync::KeyCount == 8);
    for (size_t i = 0; i < allKeys.size(); ++i) {
        CHECK(static_cast<size_t>(allKeys[i]) == i);
        CHECK(sync::mode(allKeys[i]) == sync::SyncMode::All);
        CHECK(sync::equal(state.get(allKeys[i]), sync::defaultValue(allKeys[i])));
    }
    for (auto key : {sync::Key::ShootCoolDown, sync::Key::MeleeCoolDown, sync::Key::DrawCoolDown}) CHECK(std::get<int64_t>(state.get(key)) == -1);
    CHECK(state.reload().type == gun::ReloadStateType::NotReloading);
    CHECK(state.reload().countDown == -1);
    CHECK(state.sprintTime() == 0);
    CHECK(sync::ProducedKeys == std::array{sync::Key::ReloadState, sync::Key::SprintTime});
    CHECK(sync::full(actorId, state).entries.size() == 2);
}

TEST_CASE("Shooter sync codec round trips empty partial and all typed values") {
    for (auto const& message : {sync::Message{actorId}, sync::full(actorId, active()), sync::full(actorId, sync::Snapshot{}, allKeys)}) {
        auto bytes = sync::encode(message);
        REQUIRE(bytes.size() <= sync::MaxPayloadSize);
        auto decoded = sync::decode(bytes);
        REQUIRE(decoded);
        CHECK(decoded.message.runtimeId == actorId);
        REQUIRE(decoded.message.entries.size() == message.entries.size());
        for (size_t i = 0; i < message.entries.size(); ++i) {
            CHECK(decoded.message.entries[i].key == message.entries[i].key);
            CHECK(sync::equal(decoded.message.entries[i].value, message.entries[i].value));
        }
    }
    auto bytes = sync::encode({actorId, {}});
    CHECK(bytes.size() == 13);
    CHECK(static_cast<unsigned char>(bytes[0]) == sync::ProtocolRevision);
    CHECK(static_cast<unsigned char>(bytes[4]) == 0x10);
    CHECK(static_cast<unsigned char>(bytes[11]) == 0xfe);
}

TEST_CASE("Shooter sync codec preserves signed int64 values reload ordinals and finite float bits") {
    for (auto number : {std::numeric_limits<int64_t>::min(), int64_t{-1}, int64_t{0}, int64_t{1}, std::numeric_limits<int64_t>::max()}) {
        for (int ordinal = 0; ordinal <= 4; ++ordinal) {
            sync::Message message{actorId, {{sync::Key::ShootCoolDown, number},
                {sync::Key::ReloadState, gun::ReloadState{*gun::reloadStateFromOrdinal(ordinal), number}}}};
            auto decoded = sync::decode(sync::encode(message));
            REQUIRE(decoded);
            CHECK(sync::equal(message.entries[0].value, decoded.message.entries[0].value));
            CHECK(sync::equal(message.entries[1].value, decoded.message.entries[1].value));
        }
    }
    for (float value : {0.f, -0.f, -1.f, std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()}) {
        auto decoded = sync::decode(sync::encode({actorId, {{sync::Key::SprintTime, value}}}));
        REQUIRE(decoded);
        CHECK(sync::equal(value, decoded.message.entries[0].value));
    }
    for (bool value : {false, true}) {
        auto decoded = sync::decode(sync::encode({actorId, {{sync::Key::IsBolting, value}, {sync::Key::IsAiming, value}}}));
        REQUIRE(decoded);
        CHECK(std::get<bool>(decoded.message.entries[0].value) == value);
        CHECK(std::get<bool>(decoded.message.entries[1].value) == value);
    }
}

TEST_CASE("Shooter sync codec rejects every truncation without a partial message") {
    auto bytes = sync::encode(sync::full(actorId, active(), allKeys));
    for (size_t length = 0; length < bytes.size(); ++length) checkRejected(bytes.substr(0, length), sync::DecodeError::Truncated);
}

TEST_CASE("Shooter sync codec rejects revision unknown duplicate count ordinal bool trailing and oversize") {
    auto bytes = sync::encode(sync::full(actorId, active()));
    auto bad = bytes; bad[0] = 2; checkRejected(bad, sync::DecodeError::Revision);
    bad = bytes; bad[13] = 8; checkRejected(bad, sync::DecodeError::UnknownKey);
    bad = bytes; bad[23] = bad[13]; checkRejected(bad, sync::DecodeError::DuplicateKey);
    bad = bytes; bad[12] = 9; checkRejected(bad, sync::DecodeError::Count);
    bad = bytes; bad[14] = 5; checkRejected(bad, sync::DecodeError::ReloadOrdinal);
    bad = sync::encode({actorId, {{sync::Key::IsAiming, false}}});
    bad[14] = 2; checkRejected(bad, sync::DecodeError::Bool);
    checkRejected(bytes + "x", sync::DecodeError::Trailing);
    checkRejected(std::string(sync::MaxPayloadSize + 1, '\0'), sync::DecodeError::Oversize);
}

TEST_CASE("Shooter sync codec rejects all nonfinite float categories before any apply") {
    for (uint32_t bits : {0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f800001u, 0xffc12345u}) {
        auto bytes = sync::encode(sync::full(actorId, active()));
        for (size_t i = 0; i < 4; ++i) bytes[24 + i] = static_cast<char>((bits >> (8 * i)) & 0xff);
        checkRejected(bytes, sync::DecodeError::NonFinite);
        sync::EntityStore store; store.begin(1);
        CHECK(store.apply(1, sync::full(actorId, active())));
        auto rejected = sync::decode(bytes);
        if (rejected) store.apply(1, rejected.message);
        CHECK(store.view(1, actorId).reload().countDown == 813);
        CHECK(store.view(1, actorId).sprintTime() == .25f);
    }
}

TEST_CASE("Shooter sync encoder and store refuse invalid synthetic messages atomically") {
    for (auto const& entry : {sync::Entry{sync::Key::SprintTime, int64_t{0}},
        sync::Entry{sync::Key::Count, false},
        sync::Entry{sync::Key::SprintTime, std::numeric_limits<float>::infinity()},
        sync::Entry{sync::Key::ReloadState, gun::ReloadState{static_cast<gun::ReloadStateType>(5), 0}}}) {
        sync::Message message{actorId, {{sync::Key::IsAiming, true}, entry}};
        CHECK_THROWS_AS(sync::encode(message), std::invalid_argument);
        sync::EntityStore store; store.begin(1);
        CHECK_FALSE(store.apply(1, message));
        CHECK(store.size() == 0);
    }
    sync::Message duplicate{actorId, {{sync::Key::IsAiming, true}, {sync::Key::IsAiming, false}}};
    CHECK_THROWS_AS(sync::encode(duplicate), std::invalid_argument);
}

TEST_CASE("Shooter sync dirty comparison uses Java Float equals and reload countdown") {
    CHECK_FALSE(sync::equal(0.f, -0.f));
    CHECK(sync::equal(std::bit_cast<float>(0x7fc00000u), std::bit_cast<float>(0x7fc01234u)));
    CHECK_FALSE(sync::equal(int64_t{0}, 0.f));
    sync::Baseline baseline;
    auto state = active();
    baseline.commit(state);
    state.set({sync::Key::ReloadState, gun::ReloadState{gun::ReloadStateType::TacticalFeeding, 812}});
    auto dirty = baseline.changed(actorId, state);
    REQUIRE(dirty.entries.size() == 1);
    CHECK(dirty.entries[0].key == sync::Key::ReloadState);
    CHECK(std::get<gun::ReloadState>(dirty.entries[0].value).countDown == 812);
    state = active(); state.set({sync::Key::SprintTime, std::nextafter(.25f, 1.f)});
    CHECK(baseline.changed(actorId, state).entries.size() == 1);
}

TEST_CASE("Shooter sync each key changes and returns to its Java default independently") {
    std::array<sync::Value, sync::KeyCount> values{int64_t{120}, int64_t{22},
        gun::ReloadState{gun::ReloadStateType::EmptyFinishing, 55}, .6f, int64_t{12}, true, .3f, true};
    for (size_t i = 0; i < allKeys.size(); ++i) {
        sync::Snapshot state;
        sync::Baseline baseline;
        CHECK(baseline.changed(actorId, state, allKeys).entries.empty());
        state.set({allKeys[i], values[i]});
        auto dirty = baseline.changed(actorId, state, allKeys);
        REQUIRE(dirty.entries.size() == 1);
        CHECK(dirty.entries[0].key == allKeys[i]);
        baseline.commit(state);
        CHECK(baseline.changed(actorId, state, allKeys).entries.empty());
        state.set({allKeys[i], sync::defaultValue(allKeys[i])});
        CHECK(baseline.changed(actorId, state, allKeys).entries.size() == 1);
    }
}

TEST_CASE("Shooter sync publishes sprint decay to exact zero without client decay") {
    sync::Baseline baseline;
    sync::Snapshot state;
    state.set({sync::Key::SprintTime, .125f}); baseline.commit(state);
    state.set({sync::Key::SprintTime, gun::advanceSprintTime(state.sprintTime(), .2f, .2f, false, false)});
    auto dirty = baseline.changed(actorId, state);
    REQUIRE(dirty.entries.size() == 1);
    CHECK(std::get<float>(dirty.entries[0].value) == 0);
    sync::EntityStore store; store.begin(1); store.apply(1, sync::full(actorId, active()));
    for (int frame = 0; frame < 100; ++frame) CHECK(store.local(1, actorId).sprintTime() == .25f);
    store.apply(1, dirty);
    CHECK(store.local(1, actorId).sprintTime() == 0);
}

TEST_CASE("Shooter sync full and temporary self-only refusal leave the dirty baseline untouched") {
    sync::Baseline baseline;
    auto state = active();
    auto before = baseline.changed(actorId, state);
    auto full = sync::full(actorId, state);
    sync::Message refusal{actorId, {{sync::Key::ReloadState, state.reload()}}};
    CHECK(sync::decode(sync::encode(full)));
    CHECK(sync::decode(sync::encode(refusal)));
    auto after = baseline.changed(actorId, state);
    REQUIRE(before.entries.size() == 2);
    REQUIRE(after.entries.size() == before.entries.size());
    for (size_t i = 0; i < before.entries.size(); ++i) CHECK(sync::equal(before.entries[i].value, after.entries[i].value));
    baseline.commit(state);
    CHECK(baseline.changed(actorId, state).entries.empty());
}

TEST_CASE("Shooter sync production routing sends owner then trackers and commits only afterwards") {
    // Synthetic native-recipient adapter; this does not prove Bedrock's visibility predicate.
    sync::Baseline baseline;
    auto state = active();
    std::vector<int> recipients;
    auto owner = [&](sync::Message const& message) {
        CHECK(message.runtimeId == actorId);
        CHECK(baseline.changed(actorId, state).entries.size() == 2);
        recipients.push_back(7);
    };
    auto tracking = [&](sync::Message const& message) {
        CHECK(message.entries.size() == 2);
        CHECK(baseline.changed(actorId, state).entries.size() == 2);
        for (int viewer : {7, 11, 12}) if (viewer != 7) recipients.push_back(viewer);
    };
    sync::publish(actorId, state, baseline, owner, tracking);
    CHECK(recipients == std::vector<int>{7, 11, 12});
    sync::publish(actorId, state, baseline, owner, tracking);
    CHECK(recipients.size() == 3); // idle emits nothing
}

TEST_CASE("Shooter sync client merges ordered partial entries and keeps local and remote views separate") {
    sync::EntityStore store; store.begin(1);
    CHECK(store.apply(1, sync::full(actorId, active())));
    CHECK(store.apply(1, {actorId, {{sync::Key::SprintTime, 0.f}}}));
    CHECK(store.local(1, actorId).reload().countDown == 813);
    CHECK(store.local(1, actorId).sprintTime() == 0);
    CHECK(store.apply(1, {42, {{sync::Key::SprintTime, .4f}}}));
    CHECK(store.local(1, actorId).sprintTime() == 0);
    CHECK(store.view(1, 42).sprintTime() == .4f);
    CHECK(store.apply(1, {actorId, {{sync::Key::ReloadState, gun::ReloadState{}}}}));
    CHECK(store.local(1, actorId).reload().countDown == -1);
    CHECK(store.local(1, 99).sprintTime() == 0);
    store.begin(1); CHECK(store.size() == 2); // same-session resets cannot lose early initial sync
}

TEST_CASE("Shooter sync client rejects retired sessions even if runtime id is reused and prunes absent actors") {
    gun::ClientWorldSession session;
    sync::EntityStore store;
    auto first = session.open(); store.begin(first);
    CHECK(store.apply(first, sync::full(actorId, active())));
    store.prune([](uint64_t) { return false; }); CHECK(store.size() == 0);
    // The engine adapter drops absent packets instead of storing them for later.
    bool exists = false;
    if (exists) store.apply(first, sync::full(actorId, active()));
    CHECK(store.size() == 0);
    session.close(); store.clear();
    CHECK_FALSE(store.apply(first, sync::full(actorId, active())));
    auto second = session.open(); store.begin(second);
    CHECK_FALSE(store.apply(first, sync::full(actorId, active())));
    CHECK_FALSE(store.apply(0, sync::full(actorId, active())));
    CHECK(store.local(second, actorId).sprintTime() == 0);
    CHECK(store.local(first, actorId).sprintTime() == 0);
    CHECK(store.apply(second, sync::full(actorId, active())));
    CHECK(store.local(first, actorId).sprintTime() == 0);
    CHECK(store.local(second, actorId).sprintTime() == .25f);
}
