#pragma once
#include <cstddef>
#include <utility>

namespace tacz::model {
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
