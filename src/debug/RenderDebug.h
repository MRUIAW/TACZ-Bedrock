#pragma once

#include <atomic>

// Render diagnostics shared by the /tacz command (server side, also in the client DLL for
// single-player) and the client renderer. Only meaningful in single-player.
namespace tacz::debug {

// First-person gun material; see GunRenderer.cpp materialName() for the list.
// 6 (moving-block alphatest) is the one confirmed to show the textured gun.
inline std::atomic<int> gunMaterial{6};

// Draw every quad with both windings, to rule out back-face culling. Off by default: the
// 0.2.3 test showed no missing faces with single-sided quads.
inline std::atomic<bool> gunTwoSided{false};

// Bumped whenever a setting changes so the renderer logs its state once more.
inline std::atomic<int> renderRevision{0};

} // namespace tacz::debug
