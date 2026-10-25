#pragma once
#include <cstddef>
#include <utility>

namespace tacz::model {
// Native input owns hold/toggle/double-tap interpretation. This gate separates its
// live latch from all consumed/predicted/network requests, never queues W taps or
// forces sprint eligibility. Resuming means exposing the current native latch.
class SprintInputGate {
public:
    enum class Phase { Open, Suppressed, ResumeNative };
    struct Decision { bool down, sprint, resumed; };
    Decision sample(bool blocked, bool nativeDown, bool nativeSprint) {
        bool resume=phase==Phase::Suppressed && !blocked && nativeDown;
        phase=blocked ? Phase::Suppressed : resume ? Phase::ResumeNative : Phase::Open;
        return {!blocked && nativeDown,!blocked && nativeSprint,resume};
    }
    Phase phase=Phase::Open;
};

template<class Flags>
void suppressSprintRequests(Flags& flags, size_t down, size_t sprint, size_t start) {
    flags.set(down,false); flags.set(sprint,false); flags.set(start,false);
    // Preserve StopSprinting and every unrelated movement/action bit.
}

// Preserve the persistent held-key latches. Only the native state update sees a
// temporary mask; releasing ADS must not need a second physical key-down event.
template<class Flags, class Fn>
void withSprintMasked(Flags& raw, Flags& processed, size_t bit, Fn&& update) {
    struct Restore {
        Flags& raw;
        Flags& processed;
        size_t bit;
        bool rawHeld, processedHeld;
        ~Restore() { raw.set(bit,rawHeld); processed.set(bit,processedHeld); }
    } restore{raw,processed,bit,raw.test(bit),processed.test(bit)};
    raw.set(bit,false); processed.set(bit,false);
    std::forward<Fn>(update)();
}
} // namespace tacz::model
