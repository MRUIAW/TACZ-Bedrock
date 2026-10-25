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
inline std::atomic<bool> freezePose{false};
inline std::atomic<bool> meshDiagnostics{false}; // expensive read-only comparisons, opt in
// 0 native depth; 1 isolated GL-style .05..16; 2 isolated .10..16. Diagnostic only.
inline std::atomic<int> gunDepthMode{0};

// Bumped whenever a setting changes so the renderer logs its state once more.
inline std::atomic<int> renderRevision{0};

// Bullet entity sync (/tacz bdebug): 0 Actor::moveTo, 1 Actor::teleport.
inline std::atomic<int> bulletMoveMode{0};

// Multiplies bullet speed, gravity and life stay as in the pack; < 1 gives slow motion.
inline std::atomic<float> bulletSpeedScale{1.0f};

} // namespace tacz::debug
