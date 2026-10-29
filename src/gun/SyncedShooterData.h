#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "gun/Reload.h"

// Game-independent counterpart of entity/sync/ModSyncedEntityData.java:14-60.
namespace tacz::gun::sync {

inline constexpr uint32_t ProtocolRevision = 1;
enum class Key : uint8_t {
    ShootCoolDown, MeleeCoolDown, ReloadState, AimingProgress,
    DrawCoolDown, IsAiming, SprintTime, IsBolting, Count
};
inline constexpr size_t KeyCount = static_cast<size_t>(Key::Count);
enum class SyncMode { All };
inline constexpr SyncMode mode(Key) { return SyncMode::All; }
inline constexpr std::array ProducedKeys{Key::ShootCoolDown, Key::MeleeCoolDown, Key::ReloadState,
    Key::AimingProgress, Key::DrawCoolDown, Key::IsAiming, Key::SprintTime, Key::IsBolting};
inline constexpr size_t MaxPayloadSize = 4 + 8 + 1 + KeyCount * (1 + 1 + 8);

using Value = std::variant<int64_t, gun::ReloadState, float, bool>;
struct Entry { Key key; Value value; };
struct Message { uint64_t runtimeId = 0; std::vector<Entry> entries; };

inline Value defaultValue(Key key) {
    switch (key) {
    case Key::ShootCoolDown: case Key::MeleeCoolDown: case Key::DrawCoolDown: return int64_t{-1};
    case Key::ReloadState: return gun::ReloadState{};
    case Key::AimingProgress: case Key::SprintTime: return 0.f;
    case Key::IsAiming: case Key::IsBolting: return false;
    default: throw std::invalid_argument("unknown shooter sync key");
    }
}

// DataHolder.java:16-24 uses Java equals, including Float.equals' signed zero
// distinction and canonical NaNs (non-finite values are never accepted on the wire).
inline uint32_t javaFloatBits(float value) {
    return std::isnan(value) ? 0x7fc00000u : std::bit_cast<uint32_t>(value);
}
inline bool equal(Value const& a, Value const& b) {
    if (a.index() != b.index()) return false;
    if (auto v = std::get_if<int64_t>(&a)) return *v == std::get<int64_t>(b);
    if (auto v = std::get_if<gun::ReloadState>(&a)) {
        auto const& other = std::get<gun::ReloadState>(b);
        return v->type == other.type && v->countDown == other.countDown; // ReloadState.java:52-55
    }
    if (auto v = std::get_if<float>(&a)) return javaFloatBits(*v) == javaFloatBits(std::get<float>(b));
    return std::get<bool>(a) == std::get<bool>(b);
}

class Snapshot {
    std::array<Value, KeyCount> values;
public:
    Snapshot() {
        for (size_t i = 0; i < KeyCount; ++i) values[i] = defaultValue(static_cast<Key>(i));
    }
    Value const& get(Key key) const { return values.at(static_cast<size_t>(key)); }
    void set(Entry const& entry) { values.at(static_cast<size_t>(entry.key)) = entry.value; }
    gun::ReloadState const& reload() const { return std::get<gun::ReloadState>(get(Key::ReloadState)); }
    float sprintTime() const { return std::get<float>(get(Key::SprintTime)); }
    int64_t shootCoolDown() const { return std::get<int64_t>(get(Key::ShootCoolDown)); }
    int64_t meleeCoolDown() const { return std::get<int64_t>(get(Key::MeleeCoolDown)); }
    int64_t drawCoolDown() const { return std::get<int64_t>(get(Key::DrawCoolDown)); }
    float aimingProgress() const { return std::get<float>(get(Key::AimingProgress)); }
    bool isAiming() const { return std::get<bool>(get(Key::IsAiming)); }
    bool isBolting() const { return std::get<bool>(get(Key::IsBolting)); }
};

inline Message full(uint64_t runtimeId, Snapshot const& state, std::span<Key const> keys = ProducedKeys) {
    Message result{runtimeId};
    for (auto key : keys) result.entries.push_back({key, state.get(key)});
    return result;
}

class Baseline {
    Snapshot previous;
public:
    Message changed(uint64_t runtimeId, Snapshot const& state, std::span<Key const> keys = ProducedKeys) const {
        Message result{runtimeId};
        for (auto key : keys) if (!equal(previous.get(key), state.get(key))) result.entries.push_back({key, state.get(key)});
        return result;
    }
    void commit(Snapshot const& state) { previous = state; }
};

// ALL = owner plus tracking recipients excluding that owner. The engine adapter
// supplies the exclusion; full/refusal sends deliberately never touch this baseline.
template<class Owner, class Tracking>
void publish(uint64_t id, Snapshot const& state, Baseline& baseline, Owner&& owner, Tracking&& tracking) {
    auto dirty = baseline.changed(id, state);
    if (!dirty.entries.empty()) { owner(dirty); tracking(dirty); }
    baseline.commit(state);
}

enum class DecodeError { None, Revision, UnknownKey, DuplicateKey, ReloadOrdinal, Truncated, Count, NonFinite, Bool, Trailing, Oversize, Type };
inline char const* errorName(DecodeError error) {
    switch (error) {
    case DecodeError::None: return "none";
    case DecodeError::Revision: return "revision";
    case DecodeError::UnknownKey: return "unknown-key";
    case DecodeError::DuplicateKey: return "duplicate-key";
    case DecodeError::ReloadOrdinal: return "reload-ordinal";
    case DecodeError::Truncated: return "truncated";
    case DecodeError::Count: return "entry-count";
    case DecodeError::NonFinite: return "non-finite";
    case DecodeError::Bool: return "bool";
    case DecodeError::Trailing: return "trailing-bytes";
    case DecodeError::Oversize: return "oversize";
    case DecodeError::Type: return "value-type";
    }
    return "unknown";
}
inline DecodeError validate(Message const& message) {
    if (message.entries.size() > KeyCount) return DecodeError::Count;
    std::array<bool, KeyCount> seen{};
    for (auto const& entry : message.entries) {
        auto index = static_cast<size_t>(entry.key);
        if (index >= KeyCount) return DecodeError::UnknownKey;
        if (seen[index]) return DecodeError::DuplicateKey;
        seen[index] = true;
        if (entry.value.index() != defaultValue(entry.key).index()) return DecodeError::Type;
        if (auto v = std::get_if<gun::ReloadState>(&entry.value); v && !reloadStateFromOrdinal(static_cast<int>(v->type))) return DecodeError::ReloadOrdinal;
        if (auto v = std::get_if<float>(&entry.value); v && !std::isfinite(*v)) return DecodeError::NonFinite;
    }
    return DecodeError::None;
}

// Fixed little-endian widths, no SDK serialization or host layout dependencies:
// u32 revision, u64 runtime id, u8 count, then {u8 key, typed value}.
namespace detail {
inline void put(std::string& bytes, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) bytes.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}
struct Reader {
    std::string_view bytes;
    size_t offset = 0;
    bool truncated = false;
    uint64_t get(size_t width) {
        if (width > bytes.size() - offset) { truncated = true; return 0; }
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i) value |= uint64_t{static_cast<uint8_t>(bytes[offset++])} << (i * 8);
        return value;
    }
};
}
inline std::string encode(Message const& message) {
    if (auto error = validate(message); error != DecodeError::None) throw std::invalid_argument(errorName(error));
    std::string bytes;
    detail::put(bytes, ProtocolRevision, 4);
    detail::put(bytes, message.runtimeId, 8);
    detail::put(bytes, message.entries.size(), 1);
    for (auto const& entry : message.entries) {
        detail::put(bytes, static_cast<uint8_t>(entry.key), 1);
        if (auto v = std::get_if<int64_t>(&entry.value)) detail::put(bytes, std::bit_cast<uint64_t>(*v), 8);
        else if (auto v = std::get_if<gun::ReloadState>(&entry.value)) {
            detail::put(bytes, static_cast<uint8_t>(v->type), 1);
            detail::put(bytes, std::bit_cast<uint64_t>(v->countDown), 8);
        } else if (auto v = std::get_if<float>(&entry.value)) detail::put(bytes, std::bit_cast<uint32_t>(*v), 4);
        else detail::put(bytes, std::get<bool>(entry.value) ? 1 : 0, 1);
    }
    return bytes;
}
struct DecodeResult {
    DecodeError error = DecodeError::None;
    Message message;
    explicit operator bool() const { return error == DecodeError::None; }
};
inline DecodeResult decode(std::string_view bytes) {
    auto fail = [](DecodeError error) { return DecodeResult{error, {}}; }; // never expose a partial packet
    if (bytes.size() > MaxPayloadSize) return fail(DecodeError::Oversize);
    detail::Reader reader{bytes};
    auto revision = reader.get(4);
    if (reader.truncated) return fail(DecodeError::Truncated);
    if (revision != ProtocolRevision) return fail(DecodeError::Revision);
    Message message{reader.get(8)};
    auto count = reader.get(1);
    if (reader.truncated) return fail(DecodeError::Truncated);
    if (count > KeyCount) return fail(DecodeError::Count);
    std::array<bool, KeyCount> seen{};
    for (size_t i = 0; i < count; ++i) {
        auto index = reader.get(1);
        if (reader.truncated) return fail(DecodeError::Truncated);
        if (index >= KeyCount) return fail(DecodeError::UnknownKey);
        if (seen[index]) return fail(DecodeError::DuplicateKey);
        seen[index] = true;
        auto key = static_cast<Key>(index);
        auto value = defaultValue(key);
        if (std::holds_alternative<int64_t>(value)) value = std::bit_cast<int64_t>(reader.get(8));
        else if (std::holds_alternative<gun::ReloadState>(value)) {
            auto ordinal = reader.get(1);
            auto countdown = std::bit_cast<int64_t>(reader.get(8));
            if (reader.truncated) return fail(DecodeError::Truncated);
            auto type = reloadStateFromOrdinal(static_cast<int>(ordinal));
            if (!type) return fail(DecodeError::ReloadOrdinal);
            value = gun::ReloadState{*type, countdown};
        } else if (std::holds_alternative<float>(value)) {
            auto v = std::bit_cast<float>(static_cast<uint32_t>(reader.get(4)));
            if (!std::isfinite(v)) return fail(DecodeError::NonFinite);
            value = v;
        } else {
            auto v = reader.get(1);
            if (v > 1) return fail(DecodeError::Bool);
            value = v != 0;
        }
        if (reader.truncated) return fail(DecodeError::Truncated);
        message.entries.push_back({key, value});
    }
    if (reader.offset != bytes.size()) return fail(DecodeError::Trailing);
    return {DecodeError::None, std::move(message)};
}

// Frame-owned. The active generation is part of the identity, even if an engine
// runtime id is reused on rejoin. Java ServerMessageUpdateEntityData.java:42-61
// merges entries only after resolving the entity; the caller performs resolution.
class EntityStore {
    uint64_t world = 0;
    std::unordered_map<uint64_t, Snapshot> entities;
public:
    void begin(uint64_t token) { if (world != token) { entities.clear(); world = token; } }
    void clear() { entities.clear(); world = 0; }
    bool apply(uint64_t token, Message const& message) {
        if (!token || token != world || validate(message) != DecodeError::None) return false;
        auto& state = entities[message.runtimeId];
        for (auto const& entry : message.entries) state.set(entry);
        return true;
    }
    Snapshot const& view(uint64_t token, uint64_t runtimeId) const {
        static Snapshot const defaults;
        if (!token || token != world) return defaults;
        auto found = entities.find(runtimeId);
        return found == entities.end() ? defaults : found->second;
    }
    Snapshot const& local(uint64_t token, uint64_t localRuntimeId) const { return view(token, localRuntimeId); }
    template<class Exists> void prune(Exists&& exists) {
        for (auto it = entities.begin(); it != entities.end();) {
            if (!exists(it->first)) it = entities.erase(it);
            else ++it;
        }
    }
    size_t size() const { return entities.size(); }
};

} // namespace tacz::gun::sync
