#pragma once

#include <bit>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include "gun/BulletPhysics.h"

// Pure protocol/state/schedules for the 0.4.15 experiment. No entity or visual count cap.
namespace tacz::gun::bullet {
enum class SyncMode { Tick, Every5 };
enum class DriveMode { Off, Drive, Filter };
inline bool placeDue(SyncMode mode, int age, bool final = false) {
    return final || age == 0 || mode == SyncMode::Tick || age % 5 == 0;
}
inline bool acceptMovement(DriveMode mode, int age) {
    return mode != DriveMode::Filter || (age > 0 && age % 5 == 0);
}
inline glm::dvec3 interpolate(glm::dvec3 previous, glm::dvec3 current, double alpha) {
    return previous + (current - previous) * alpha;
}
enum class Kind : uint8_t { Spawn = 1, Remove = 2 };
struct Message {
    Kind kind = Kind::Spawn;
    uint64_t serial = 0, runtime = 0;
    int64_t unique = 0;
    int32_t dimension = 0;
    BulletMotion motion;
    bool waterRules = true;
};
inline constexpr size_t WireSize = 91;
inline std::string encode(Message const& m) {
    std::string out;
    auto put = [&]<class T>(T value) {
        auto bytes = std::bit_cast<std::array<unsigned char, sizeof(T)>>(value);
        // Protocol is explicitly little-endian; supported targets are Windows x64.
        for (auto b : bytes) out.push_back(static_cast<char>(b));
    };
    put(uint8_t{1}); put(static_cast<uint8_t>(m.kind)); put(m.serial); put(m.runtime);
    put(m.unique); put(m.dimension);
    for (int i = 0; i < 3; ++i) put(m.motion.pos[i]);
    for (int i = 0; i < 3; ++i) put(m.motion.vel[i]);
    put(m.motion.gravity); put(m.motion.friction); put(static_cast<int32_t>(m.motion.life));
    put(static_cast<uint8_t>(m.waterRules));
    return out;
}
inline std::optional<Message> decode(std::string_view bytes) {
    if (bytes.size() != WireSize) return {};
    size_t cursor = 0;
    auto get = [&]<class T>() {
        std::array<unsigned char, sizeof(T)> data{};
        for (auto& b : data) b = static_cast<unsigned char>(bytes[cursor++]);
        return std::bit_cast<T>(data);
    };
    if (get.operator()<uint8_t>() != 1) return {};
    auto kind = get.operator()<uint8_t>();
    if (kind < 1 || kind > 2) return {};
    Message m; m.kind = static_cast<Kind>(kind);
    m.serial = get.operator()<uint64_t>(); m.runtime = get.operator()<uint64_t>();
    m.unique = get.operator()<int64_t>(); m.dimension = get.operator()<int32_t>();
    for (int i = 0; i < 3; ++i) m.motion.pos[i] = get.operator()<double>();
    for (int i = 0; i < 3; ++i) m.motion.vel[i] = get.operator()<double>();
    m.motion.gravity = get.operator()<float>(); m.motion.friction = get.operator()<float>();
    m.motion.life = get.operator()<int32_t>();
    auto water = get.operator()<uint8_t>();
    if (!m.serial || !m.runtime || m.motion.life < 1 || water > 1
        || !std::isfinite(m.motion.gravity) || m.motion.gravity < 0
        || !std::isfinite(m.motion.friction) || m.motion.friction < 0) return {};
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(m.motion.pos[i]) || !std::isfinite(m.motion.vel[i])) return {};
    m.waterRules = water != 0;
    return m;
}
struct State {
    struct Receipt { bool filtered; std::optional<double> snap; };
    Message spawn;
    BulletMotion motion;
    glm::dvec3 previous{}, lastServer{}, lastWritten{};
    bool wrote = false, diagnostic = false;
    unsigned ticks = 0, overwrites = 0, unexplained = 0, received = 0, filtered = 0;
    unsigned absolute = 0, delta = 0, hints = 0, nativeMotion = 0;
    unsigned correctionLines = 0;
    unsigned arrivedTick = 0, filteredTick = 0, acceptedTick = 0;
    double maxSnap = 0, tickSnap = 0;
    explicit State(Message const& m) : spawn(m), motion(m.motion), previous(m.motion.pos), lastServer(m.motion.pos) {}
    double snap(glm::dvec3 position) {
        auto distance = glm::length(position - motion.pos);
        motion.pos = position; // Java Entity.lerpTo: snap current only; keep velocity and previous.
        maxSnap = std::max(maxSnap, distance); tickSnap = std::max(tickSnap, distance);
        return distance;
    }
    bool step(bool water) {
        previous = motion.pos; ++ticks;
        return motion.step(spawn.waterRules && water);
    }
    Receipt receive(DriveMode mode, int kind, glm::dvec3 position, std::array<bool,3> axes) {
        ++received; ++arrivedTick;
        if (kind == 0) ++absolute; else if (kind == 1) ++delta;
        else if (kind == 2) ++nativeMotion; else ++hints;
        // All received axes, not just accepted ones, advance the transport baseline.
        for (int i = 0; i < 3; ++i) if (axes[i]) lastServer[i] = position[i];
        bool reject = !acceptMovement(mode,motion.age);
        if (reject) { ++filtered; ++filteredTick; return {true,{}}; }
        ++acceptedTick;
        if (axes[0] || axes[1] || axes[2]) return {false,snap(lastServer)};
        return {false,{}}; // hints/motion do not change our Java velocity or position
    }
};
class States {
    uint64_t lastSpawn = 0;
    std::unordered_map<uint64_t, uint64_t> runtimes;
public:
    std::unordered_map<uint64_t, State> entries;
    bool spawn(Message const& m) {
        // Ordered reliable custom stream. Reject duplicates/late spawns without tombstone growth.
        if (m.kind != Kind::Spawn || m.serial <= lastSpawn || runtimes.contains(m.runtime)) return false;
        lastSpawn = m.serial;
        entries.emplace(m.serial, m); runtimes.emplace(m.runtime, m.serial); return true;
    }
    State* runtime(uint64_t id) {
        auto it = runtimes.find(id);
        if (it == runtimes.end()) return nullptr;
        return &entries.at(it->second);
    }
    bool remove(uint64_t serial) {
        auto it = entries.find(serial); if (it == entries.end()) return false;
        runtimes.erase(it->second.spawn.runtime); entries.erase(it); return true;
    }
    bool correct(uint64_t serial, glm::dvec3 pos) {
        auto it = entries.find(serial); if (it == entries.end()) return false;
        it->second.snap(pos); return true;
    }
    void clear() { entries.clear(); runtimes.clear(); lastSpawn = 0; }
};
// Bounded diagnostics only, never a projectile/visibility cap. Re-arm for another sample.
struct DiagnosticBudget {
    unsigned remaining = 0;
    uint64_t until = 0;
    void arm(unsigned n, uint64_t tick) { remaining = std::min(n, 16u); until = tick + 400; }
    bool take(uint64_t tick) {
        if (!remaining || tick >= until) return false;
        --remaining; return true;
    }
};
} // namespace tacz::gun::bullet
