#pragma once

#include <cstdint>
#include <optional>

// Reload state, as TACZ's api/entity/ReloadState. The ordinals are what Lua scripts see.
namespace tacz::gun {

enum class ReloadStateType : int {
    NotReloading      = 0,
    EmptyFeeding      = 1,
    EmptyFinishing    = 2,
    TacticalFeeding   = 3,
    TacticalFinishing = 4,
};

inline bool isReloadingEmpty(ReloadStateType t) {
    return t == ReloadStateType::EmptyFeeding || t == ReloadStateType::EmptyFinishing;
}
inline bool isReloadingTactical(ReloadStateType t) {
    return t == ReloadStateType::TacticalFeeding || t == ReloadStateType::TacticalFinishing;
}
inline bool isReloading(ReloadStateType t) { return isReloadingEmpty(t) || isReloadingTactical(t); }
inline bool isReloadFinishing(ReloadStateType t) {
    return t == ReloadStateType::EmptyFinishing || t == ReloadStateType::TacticalFinishing;
}

inline std::optional<ReloadStateType> reloadStateFromOrdinal(int ordinal) {
    if (ordinal < 0 || ordinal > 4) return std::nullopt;
    return static_cast<ReloadStateType>(ordinal);
}

// TACZ enum names, also used as Lua global constants.
inline char const* reloadStateName(ReloadStateType t) {
    switch (t) {
    case ReloadStateType::NotReloading:
        return "NOT_RELOADING";
    case ReloadStateType::EmptyFeeding:
        return "EMPTY_RELOAD_FEEDING";
    case ReloadStateType::EmptyFinishing:
        return "EMPTY_RELOAD_FINISHING";
    case ReloadStateType::TacticalFeeding:
        return "TACTICAL_RELOAD_FEEDING";
    case ReloadStateType::TacticalFinishing:
        return "TACTICAL_RELOAD_FINISHING";
    }
    return "?";
}

struct ReloadState {
    ReloadStateType type      = ReloadStateType::NotReloading;
    int64_t         countDown = -1; // ms left in the current state; -1 when not reloading
};

} // namespace tacz::gun
