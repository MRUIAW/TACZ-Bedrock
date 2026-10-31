#pragma once

#include <bit>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "gun/BulletPhysics.h"

// Production bullet protocol/state: five-tick server placement, always-on client driving.
// Filename retained for history. No entity or visual count cap.
namespace tacz::gun::bullet {
inline bool correctionAge(int positionAge) { return positionAge > 0 && positionAge % 5 == 0; }
inline bool placeDue(int age, bool final = false) {
    return final || age == 0 || correctionAge(age);
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
// Metadata only: native MoveActor packets remain the source of corrections.
// Send BEFORE placing so a receiver need not infer position age from arrival time.
struct Placement {
    uint64_t serial = 0, runtime = 0;
    int64_t unique = 0;
    int32_t dimension = 0, age = 0;
    glm::vec3 pos{}; // exact float target used by the native actor/packet
};
inline constexpr size_t PlacementWireSize = 45;
inline std::string encodePlacement(Placement const& m) {
    std::string out;
    auto put = [&]<class T>(T value) {
        for (auto b : std::bit_cast<std::array<unsigned char,sizeof(T)>>(value)) out.push_back(static_cast<char>(b));
    };
    put(uint8_t{1}); put(m.serial); put(m.runtime); put(m.unique); put(m.dimension); put(m.age);
    for (int i = 0; i < 3; ++i) put(m.pos[i]);
    return out;
}
inline std::optional<Placement> decodePlacement(std::string_view bytes) {
    if (bytes.size() != PlacementWireSize) return {};
    size_t cursor = 0;
    auto get = [&]<class T>() {
        std::array<unsigned char,sizeof(T)> data{};
        for (auto& b : data) b = static_cast<unsigned char>(bytes[cursor++]);
        return std::bit_cast<T>(data);
    };
    if (get.operator()<uint8_t>() != 1) return {};
    Placement m;
    m.serial = get.operator()<uint64_t>(); m.runtime = get.operator()<uint64_t>(); m.unique = get.operator()<int64_t>();
    m.dimension = get.operator()<int32_t>(); m.age = get.operator()<int32_t>();
    for (int i = 0; i < 3; ++i) m.pos[i] = get.operator()<float>();
    if (!m.serial || !m.runtime || m.age < 0) return {};
    for (int i = 0; i < 3; ++i) if (!std::isfinite(m.pos[i])) return {};
    return m;
}
// Uses the real SDK component in production and a plain component in synthetic tests.
template <class Component, class Position, class Rotation>
void overrideInterpolator(Component& component, Position const& pos, Rotation const& rot) {
    component.mPos = pos; component.mRot = rot; component.mHeadYaw = rot.y;
    component.mPositionSteps = 0; component.mRotationSteps = 0; component.mHeadYawSteps = 0;
    // Do not change native completion/vehicle ownership flags.
}
struct State {
    struct Correction { int age, kind; glm::dvec3 pos; };
    struct Applied { Correction correction; double snap; bool afterStep; };
    Message spawn;
    BulletMotion motion;
    glm::dvec3 previous{}, lastServer{}, lastWritten{};
    bool wrote = false, diagnostic = false;
    bool interpolatorMissingLogged = false;
    unsigned ticks = 0, overwrites = 0, unexplained = 0, received = 0;
    unsigned absolute = 0, delta = 0;
    unsigned unaged = 0;
    int lastPacketAge = -1;
    unsigned correctionLines = 0;
    unsigned arrivedTick = 0, acceptedTick = 0;
    double maxSnap = 0, tickSnap = 0;
    std::vector<std::pair<int,glm::vec3>> placements;
    std::vector<Correction> pending;
    std::vector<Applied> applied;
    explicit State(Message const& m) : spawn(m), motion(m.motion), previous(m.motion.pos), lastServer(m.motion.pos) {
        placements.emplace_back(0,glm::vec3(m.motion.pos));
    }
    void placement(int age, glm::vec3 pos) { placements.emplace_back(age,pos); }
    std::optional<int> ageOf(glm::dvec3 pos) const {
        auto native = glm::vec3(pos);
        for (auto it = placements.rbegin(); it != placements.rend(); ++it) if (it->second == native) return it->first;
        return {};
    }
    double snap(glm::dvec3 position) {
        auto distance = glm::length(position - motion.pos);
        motion.pos = position; // Java Entity.lerpTo: snap current only; keep velocity and previous.
        maxSnap = std::max(maxSnap, distance); tickSnap = std::max(tickSnap, distance);
        return distance;
    }
    template <class WaterAt> bool tick(WaterAt&& waterAt) {
        applied.clear();
        // Packets can arrive on either side of the step that owns their position.
        // Never fast-forward age/velocity or add a second physics step.
        size_t after = 0;
        while (after < pending.size() && pending[after].age <= motion.age) {
            auto const& c = pending[after++]; applied.push_back({c,snap(c.pos),false});
        }
        previous = motion.pos; ++ticks;
        bool water = waterAt(motion.pos);
        bool alive = motion.step(spawn.waterRules && water);
        for (; after < pending.size(); ++after) {
            auto const& c = pending[after]; applied.push_back({c,snap(c.pos),true});
        }
        pending.clear();
        return alive;
    }
    bool step(bool water) { return tick([water](glm::dvec3) { return water; }); }
    bool observeBefore(glm::dvec3 pos) {
        bool changed = wrote && glm::length(pos-lastWritten) > .0001;
        if (changed) { ++overwrites; if (!acceptedTick) ++unexplained; }
        return changed;
    }
    void receive(int kind, glm::dvec3 position, std::array<bool,3> axes) {
        ++received; ++arrivedTick;
        if (kind == 0) ++absolute; else ++delta;
        // Every position packet passes through to Bedrock; partial axes retain the baseline.
        for (int i = 0; i < 3; ++i) if (axes[i]) lastServer[i] = position[i];
        bool hasPosition = axes[0] || axes[1] || axes[2];
        auto positionAge = ageOf(lastServer); // rotation/motion-only packets retain the last received position's age
        lastPacketAge = positionAge.value_or(-1);
        if (!positionAge) ++unaged; // no guessed local-age fallback
        ++acceptedTick;
        // Honor authoritative corrections even if metadata is missing. Age -1 retains
        // Java's before-step snap, never a guessed age.
        if (hasPosition) pending.push_back({positionAge.value_or(-1),kind,lastServer});
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
// Module capability/settings outlive a world; only records are cleared on rejoin.
// `available` is set by startup hook validation, never a player-selectable drive mode.
struct ClientDriver {
    States bullets;
    bool available = false;
    bool marker = false;
    void clearWorld() { bullets.clear(); }
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
