#pragma once
// Historical 0.3.28 measurement model retained for regression tests ONLY.
// No per-caller types/instances/statistics remain in production bridge code.
#include "pack/DescriptorProbe.h"
#include <utility>
namespace tacz::pack::probe {
struct Caller {
    uintptr_t address=0; uint64_t thread=0; bool nativeImage=false; int route=0;
    auto operator<=>(Caller const&) const=default;
};
struct Counts {
    uint64_t calls=0,resolved=0,different=0,copyHits=0;
    std::map<int,uint64_t> native,would;
    std::map<std::string,uint64_t> status,ammo;
    void add(int value,Lookup const& hit) {
        ++calls; ++native[value]; ++status[resolutionName(hit.status)];
        if (hit.record) {
            ++resolved; ++would[hit.record->limit]; different+=value!=hit.record->limit; copyHits+=hit.record->copied;
            if (ammo.contains(hit.record->ammo) || ammo.size()<8) ++ammo[hit.record->ammo]; else ++ammo["<other>"];
        }
    }
};
class Samples {
public:
    static constexpr size_t Capacity=1024;
    std::map<Caller,Counts> window,total;
    uint64_t dropped=0;
    bool add(Caller caller,int value,Lookup const& hit) {
        if (!total.contains(caller) && total.size()>=Capacity) { ++dropped; return false; }
        total[caller].add(value,hit); window[caller].add(value,hit); return true;
    }
    std::map<Caller,Counts> takeWindow() { return std::exchange(window,{}); }
    void clear() { window.clear(); total.clear(); dropped=0; }
};
}
