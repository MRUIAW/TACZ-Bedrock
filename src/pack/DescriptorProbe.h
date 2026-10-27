#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace tacz::pack::probe {
enum class Resolution { Candidate,Missing,Expired,FingerprintMismatch };
char const* resolutionName(Resolution);
struct DescriptorRecord {
    uintptr_t item=0;
    int aux=0,limit=0;
    std::string ammo;
    uint64_t bornMs=0,producer=0;
    bool copied=false;
};
struct Lookup { Resolution status=Resolution::Missing; std::optional<DescriptorRecord> record; };
// Pointer values are opaque keys ONLY, never retained native references or
// dereferenced allocations. Candidate != a proven lifetime-safe identity.
class DescriptorBridge {
    using Key=std::pair<uint64_t,uintptr_t>; // thread + BaseDescriptor allocation
    std::map<Key,DescriptorRecord> entries;
public:
    static constexpr uint64_t LifetimeMs=250;
    static constexpr size_t Capacity=4096;
    uint64_t invalidated=0,capacityCleared=0,published=0,copied=0;
    void erase(uint64_t thread,uintptr_t impl);
    void put(uint64_t thread,uintptr_t impl,DescriptorRecord record);
    Lookup lookup(uint64_t thread,uintptr_t impl,uintptr_t item,int aux,uint64_t now) const;
    void clear();
    size_t size() const { return entries.size(); }
};
struct Caller {
    uintptr_t address=0; // RVA when nativeImage, otherwise explicit absolute address
    uint64_t thread=0;
    bool nativeImage=false;
    int route=0; // Item / ComponentItem
    auto operator<=>(Caller const&) const=default;
};
struct Counts {
    uint64_t calls=0,resolved=0,different=0,copyHits=0;
    std::map<int,uint64_t> native,would;
    std::map<std::string,uint64_t> status,ammo;
    void add(int nativeValue,Lookup const& lookup);
};
class Samples {
public:
    static constexpr size_t Capacity=1024;
    std::map<Caller,Counts> window,total;
    uint64_t dropped=0;
    bool add(Caller caller,int nativeValue,Lookup const& lookup);
    std::map<Caller,Counts> takeWindow();
    void clear();
};
}
