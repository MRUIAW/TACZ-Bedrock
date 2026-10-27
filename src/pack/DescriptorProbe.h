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
    uint64_t bornMs=0;
    bool copied=false;
};
struct Lookup { Resolution status=Resolution::Missing; std::optional<DescriptorRecord> record; };
int descriptorLimit(Lookup const&,int fallback);
// Pointer values are opaque keys ONLY, never retained native references or
// dereferenced allocations. Candidate != a proven lifetime-safe identity.
class DescriptorBridge {
    using Key=std::pair<uint64_t,uintptr_t>; // thread + BaseDescriptor allocation
    std::map<Key,DescriptorRecord> entries;
public:
    static constexpr uint64_t LifetimeMs=250;
    static constexpr size_t Capacity=4096;
    void erase(uint64_t thread,uintptr_t impl);
    void put(uint64_t thread,uintptr_t impl,DescriptorRecord record);
    Lookup lookup(uint64_t thread,uintptr_t impl,uintptr_t item,int aux,uint64_t now) const;
    void clear();
    size_t size() const { return entries.size(); }
};
}
