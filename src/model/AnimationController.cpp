#include "model/AnimationController.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include <glm/gtc/quaternion.hpp>

namespace tacz::animation {
namespace {
using Value = std::optional<glm::vec3>;
Value& value(BonePose& b, ChannelType t) {
    return t == ChannelType::Translation ? b.translation : t == ChannelType::Rotation ? b.rotation : b.scale;
}

glm::vec3 angles(glm::quat q) {
    return {
        std::atan2(2.f*(q.w*q.x+q.y*q.z), 1.f-2.f*(q.x*q.x+q.y*q.y)),
        std::asin(std::clamp(2.f*(q.w*q.y-q.x*q.z), -1.f, 1.f)),
        std::atan2(2.f*(q.w*q.z+q.y*q.x), 1.f-2.f*(q.y*q.y+q.z*q.z))
    };
}

glm::vec3 interpolate(glm::vec3 a, glm::vec3 b, ChannelType type, float alpha) {
    if (type != ChannelType::Rotation) return a * (1.f - alpha) + b * alpha;
    auto qa = glm::quat(a), qb = glm::quat(b);
    float dot = glm::dot(qa, qb);
    if (dot < 0) { qb = -qb; dot = -dot; }
    float s0 = 1.f-alpha, s1 = alpha;
    if (1.f-dot > 1e-6f) {
        float omega = std::acos(std::clamp(dot, -1.f, 1.f));
        float inv = 1.f/std::sin(omega);
        s0 = std::sin((1.f-alpha)*omega)*inv;
        s1 = std::sin(alpha*omega)*inv;
    }
    return angles(qa*s0 + qb*s1); // ObjectAnimationRunner.slerp, not Euler lerp
}

struct Runner {
    std::string name;
    Animation const* clip;
    PlayType type;
    double progress = 0;
    double lastUpdate = 0;
    bool running = true, pausing = false, holding = false, stopped = false;
    std::unique_ptr<Runner> to;
    bool transitioning = false;
    double transitionTime = 0, transitionProgress = 0;
    Pose from, recover;
    Pose const* initial = nullptr;

    void hold() { progress = clip->duration + 1e-9; running = false; holding = true; stopped = false; }
    void stop() { progress = clip->duration + 2e-9; running = false; holding = false; stopped = true; }
    void seek(double p) { progress = p; holding = stopped = false; }

    Pose sample() const {
        Pose pose;
        evaluate(*clip, static_cast<float>(progress), pose);
        return pose;
    }

    Pose transitionPose() const {
        Pose pose;
        auto target = to->sample();
        // TACZ calls this easeOutCubic, but the actual exponent is FOUR.
        float alpha = static_cast<float>(1.0 - std::pow(1.0-transitionProgress/transitionTime, 4));
        for (auto const& [name, b] : from) {
            BonePose copy = b;
            auto& result = pose[name];
            for (auto t : {ChannelType::Translation, ChannelType::Rotation, ChannelType::Scale}) {
                auto const& a = value(copy, t);
                auto const& end = value(target[name], t);
                if (a && end) value(result, t) = interpolate(*a, *end, t, alpha);
            }
        }
        if (type != PlayType::Stop) {
            for (auto const& [name, b] : recover) {
                BonePose copy = b;
                for (auto t : {ChannelType::Translation, ChannelType::Rotation, ChannelType::Scale}) {
                    if (auto const& a = value(copy, t)) {
                        glm::vec3 end(t == ChannelType::Scale ? 1.f : 0.f);
                        if (initial) if (auto it = initial->find(name); it != initial->end()) {
                            BonePose defaults = it->second;
                            if (auto v = value(defaults,t)) end = *v;
                        }
                        value(pose[name], t) = interpolate(*a, end, t, alpha);
                    }
                }
            }
        }
        return pose;
    }

    void transition(std::unique_ptr<Runner> next, double duration) {
        // A repeated transition snapshots only the old matched channels, just as TACZ's
        // transitionFromChannels does (old recovery / target-only channels are not retained).
        Pose start = to && transitioning ? transitionPose() : sample();
        if (to && transitioning) {
            for (auto it = start.begin(); it != start.end();) {
                auto old = from.find(it->first);
                if (old == from.end()) { it = start.erase(it); continue; }
                for (auto t : {ChannelType::Translation, ChannelType::Rotation, ChannelType::Scale}) {
                    if (!value(old->second, t)) value(it->second, t).reset();
                }
                ++it;
            }
        }
        from.clear();
        recover.clear();
        auto destination = next->sample();
        for (auto& [name, b] : start) {
            for (auto t : {ChannelType::Translation, ChannelType::Rotation, ChannelType::Scale}) {
                if (auto const& v = value(b, t)) {
                    value(value(destination[name], t) ? from[name] : recover[name], t) = *v;
                }
            }
        }
        running = false;
        to = std::move(next);
        transitioning = true;
        transitionTime = duration;
        transitionProgress = 0;
    }

    void update(double now, bool blend, Pose& pose, Controller::SoundFn const& sounds, Pose const* suppressed = nullptr) {
        double delta = std::max(now-lastUpdate, 0.0);
        lastUpdate = now;
        double previous = progress;
        if (running) progress += delta;
        if (progress > clip->duration) {
            switch (type) {
            case PlayType::Hold: hold(); break;
            case PlayType::Stop: stop(); break;
            case PlayType::Loop:
                progress = clip->duration == 0 ? 0 : std::fmod(progress, clip->duration);
                holding = stopped = false;
                break;
            }
        }
        Pose result;
        if (transitioning) {
            transitionProgress += delta;
            if (transitionProgress >= transitionTime) transitioning = false;
            else result = transitionPose();
        } else {
            result = sample();
            if (sounds && previous != progress) sounds(name, previous, progress);
        }
        if (suppressed) {
            for (auto const& [name, b] : *suppressed) {
                BonePose copy = b;
                for (auto t : {ChannelType::Translation, ChannelType::Rotation, ChannelType::Scale}) {
                    if (value(copy, t)) value(result[name], t).reset();
                }
            }
        }
        compose(result, pose, blend);
    }
};
} // namespace

struct Controller::Impl {
    AnimationFile const& animations;
    SoundFn sounds;
    double now = 0;
    Pose initial;
    std::vector<std::vector<int>> lines;
    std::vector<std::unique_ptr<Runner>> tracks;
    std::vector<bool> blending;

    Impl(AnimationFile const& a, SoundFn s) : animations(a), sounds(std::move(s)) {}
    Runner* root(int t) const { return t >= 0 && t < static_cast<int>(tracks.size()) ? tracks[t].get() : nullptr; }
    Runner* target(int t) const {
        auto r = root(t);
        return r && r->to ? r->to.get() : r;
    }
    void checkLine(int l) const {
        if (l < 0 || l >= static_cast<int>(lines.size())) throw std::out_of_range("animation track line");
    }
};

Controller::Controller(AnimationFile const& a, SoundFn s) : mImpl(std::make_unique<Impl>(a, std::move(s))) {}
Controller::~Controller() = default;
void Controller::setTime(double now) {
    if (!std::isfinite(now)) throw std::invalid_argument("animation clock");
    mImpl->now = now;
}
void Controller::setInitialPose(Pose pose) { mImpl->initial = std::move(pose); }
int Controller::addTrackLine() { int line = getTrackLineSize(); ensureTrackLineSize(line+1); return line; }
void Controller::ensureTrackLineSize(int size) {
    if (size < 0 || size > 64) throw std::out_of_range("animation line limit (64)");
    if (size > getTrackLineSize()) mImpl->lines.resize(size);
}
int Controller::getTrackLineSize() const { return static_cast<int>(mImpl->lines.size()); }
int Controller::assignNewTrack(int line) {
    mImpl->checkLine(line);
    int track = static_cast<int>(mImpl->tracks.size());
    if (track >= 4096) throw std::out_of_range("animation track limit (4096)");
    mImpl->lines[line].push_back(track);
    mImpl->tracks.emplace_back();
    mImpl->blending.push_back(false);
    return track;
}
int Controller::findIdleTrack(int line, bool interruptHolding) {
    mImpl->checkLine(line);
    for (auto t : mImpl->lines[line]) {
        auto r = mImpl->root(t);
        if (!r || r->stopped || (interruptHolding && r->holding)) return t;
    }
    return assignNewTrack(line);
}
void Controller::ensureTracksAmount(int line, int amount) {
    mImpl->checkLine(line);
    if (amount < 0 || amount > 4096) throw std::out_of_range("animation track amount");
    while (static_cast<int>(mImpl->lines[line].size()) < amount) assignNewTrack(line);
}
int Controller::getTrack(int line, int index) const {
    if (line < 0 || line >= getTrackLineSize() || index < 0 || index >= static_cast<int>(mImpl->lines[line].size())) return -1;
    return mImpl->lines[line][index];
}
int Controller::getAsSingletonTrack(int line) { return getTrack(line, 0) < 0 ? assignNewTrack(line) : getTrack(line, 0); }
void Controller::runAnimation(std::string const& name, int track, bool blend, PlayType type, float transition) {
    if (track < 0 || track >= static_cast<int>(mImpl->tracks.size())) throw std::out_of_range("animation track");
    // AnimationStateContext calls setBlending even when runAnimation finds no prototype.
    mImpl->blending[track] = blend;
    auto it = mImpl->animations.find(name);
    if (it == mImpl->animations.end()) return; // TACZ silently ignores missing prototypes
    if (!std::isfinite(transition)) throw std::invalid_argument("animation transition time");
    auto runner = std::make_unique<Runner>();
    runner->initial = &mImpl->initial;
    runner->name = name;
    runner->clip = &it->second;
    runner->type = type;
    runner->lastUpdate = mImpl->now;
    auto& root = mImpl->tracks[track];
    if (root && transition > 0) root->transition(std::move(runner), transition);
    else root = std::move(runner);
    mImpl->blending[track] = blend;
}
void Controller::stopAnimation(int track) { if (auto r = mImpl->root(track)) r->stop(); }
void Controller::holdAnimation(int track) { if (auto r = mImpl->root(track)) r->hold(); }
void Controller::pauseAnimation(int track) { if (auto r = mImpl->root(track)) { r->running = false; r->pausing = true; } }
void Controller::resumeAnimation(int track) {
    if (auto r = mImpl->root(track)) {
        if (!r->running) r->lastUpdate = mImpl->now;
        r->running = true;
        r->pausing = false;
    }
}
void Controller::setAnimationProgress(int track, float progress, bool normalized) {
    if (!std::isfinite(progress)) throw std::invalid_argument("animation progress");
    if (auto r = mImpl->root(track)) {
        if (!r->running && !r->pausing) r = r->to.get();
        if (r) r->seek(progress * (normalized ? r->clip->duration : 1.f));
    }
}
void Controller::adjustAnimationProgress(int track, float progress, bool normalized) {
    if (!std::isfinite(progress)) throw std::invalid_argument("animation progress");
    if (auto r = mImpl->root(track)) {
        if (!r->running) r = r->to.get();
        if (r) r->seek(r->progress + progress * (normalized ? r->clip->duration : 1.f));
    }
}
bool Controller::hasAnimationPrototype(std::string const& name) const { return mImpl->animations.contains(name); }
Controller::TrackInfo Controller::info(int track) const {
    TrackInfo result;
    if (auto r = mImpl->target(track)) {
        result = {r->name, r->progress, r->holding, r->stopped, r->pausing, mImpl->root(track)->transitioning};
    }
    return result;
}
Pose Controller::update(double now) {
    setTime(now);
    Pose pose;
    for (auto const& line : mImpl->lines) {
        for (int t : line) {
            auto& root = mImpl->tracks[t];
            if (!root) continue;
            bool blend = mImpl->blending[t];
            if (root->running || root->holding || root->pausing || root->transitioning) root->update(now, blend, pose, mImpl->sounds);
            if (root->to) {
                root->to->update(now, blend, pose, mImpl->sounds, root->transitioning ? &root->from : nullptr);
                if (!root->transitioning) {
                    auto next = std::move(root->to);
                    root = std::move(next);
                }
            }
        }
    }
    return pose;
}
void Controller::clear() { mImpl->lines.clear(); mImpl->tracks.clear(); mImpl->blending.clear(); }

} // namespace tacz::animation
