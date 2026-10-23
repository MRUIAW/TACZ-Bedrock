#pragma once

#include <memory>
#include <string>
#include <vector>

#include "model/Animation.h"

namespace tacz::animation {

// AnimationController + DiscreteTrackArray + ObjectAnimationRunner. The caller supplies a
// monotonic clock in seconds; tests can advance it without sleeping or loading Minecraft.
class Controller {
public:
    using SoundFn = std::function<void(std::string const& animation, double from, double to)>;
    struct TrackInfo {
        std::string name; // transition target, as the context's isHolding/isStopped queries
        double progress = 0;
        bool holding = false;
        bool stopped = true;
        bool pausing = false;
        bool transitioning = false;
    };

    explicit Controller(AnimationFile const& animations, SoundFn sounds = {});
    ~Controller();
    Controller(Controller const&) = delete;
    Controller& operator=(Controller const&) = delete;

    void setTime(double now);
    int addTrackLine();
    void ensureTrackLineSize(int size);
    int getTrackLineSize() const;
    int assignNewTrack(int line);
    int findIdleTrack(int line, bool interruptHolding);
    void ensureTracksAmount(int line, int amount);
    int getTrack(int line, int index) const;
    int getAsSingletonTrack(int line);
    void runAnimation(std::string const& name, int track, bool blending, PlayType type, float transition);
    void stopAnimation(int track);
    void holdAnimation(int track);
    void pauseAnimation(int track);
    void resumeAnimation(int track);
    void setAnimationProgress(int track, float progress, bool normalized);
    void adjustAnimationProgress(int track, float progress, bool normalized);
    bool hasAnimationPrototype(std::string const& name) const;
    TrackInfo info(int track) const;
    Pose update(double now);
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace tacz::animation
