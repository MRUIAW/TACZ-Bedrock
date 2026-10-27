#pragma once
#include <atomic>
namespace tacz::debug {
// Opt-in, bounded independently so firing cannot hide client receive evidence.
class AmmoSyncBudget {
    std::atomic<unsigned> remaining{0};
public:
    void arm(bool enabled) { remaining=enabled ? 256u : 0u; }
    bool available() const { return remaining.load(std::memory_order_relaxed)!=0; }
    bool take() {
        auto n=remaining.load(std::memory_order_relaxed);
        while (n && !remaining.compare_exchange_weak(n,n-1,std::memory_order_relaxed)) {}
        return n!=0;
    }
};
inline std::atomic<bool> ammoSync{false};
inline AmmoSyncBudget ammoServerSamples,ammoClientSamples,ammoPickupSamples,ammoPacketSamples;
inline void setAmmoSync(bool enabled) {
    ammoSync=false;
    ammoServerSamples.arm(enabled); ammoClientSamples.arm(enabled); ammoPickupSamples.arm(enabled);
    ammoPacketSamples.arm(enabled);
    ammoSync=enabled;
}
}
