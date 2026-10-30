#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include "pack/ResourceId.h"

namespace tacz::gun {

inline int64_t addTime(int64_t a, int64_t b) {
    return std::bit_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}

// Java baseTimestamp translates client shot timestamps to wall time. Our offset
// translates to steady time, so add the wall/steady delta. Before synchronization
// the data holder's construction-time wall base is used instead.
inline int64_t shotWallBase(std::optional<int64_t> offset, int64_t wall, int64_t steady, int64_t initialBase) {
    return offset ? addTime(*offset, wall - steady) : initialBase;
}

struct HoldIdentity {
    uint64_t generation = 0;
    int slot = -1;
    pack::ResourceId gunId;
    auto operator<=>(HoldIdentity const&) const = default;
};

} // namespace tacz::gun
