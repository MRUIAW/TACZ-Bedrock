#include "pack/DescriptorProbe.h"
#include <utility>
namespace tacz::pack::probe {
int descriptorLimit(Lookup const& hit,int fallback) {
    return hit.status==Resolution::Candidate && hit.record && hit.record->limit>0 ? hit.record->limit : fallback;
}
char const* resolutionName(Resolution r) {
    switch(r) {
    case Resolution::Candidate:return "candidate";
    case Resolution::Expired:return "expired";
    case Resolution::FingerprintMismatch:return "fingerprint-mismatch";
    default:return "missing";
    }
}
void DescriptorBridge::erase(uint64_t thread,uintptr_t impl) { entries.erase({thread,impl}); }
void DescriptorBridge::put(uint64_t thread,uintptr_t impl,DescriptorRecord record) {
    if (!impl) return;
    erase(thread,impl);
    if (entries.size()>=Capacity) entries.clear();
    entries.emplace(Key{thread,impl},std::move(record));
}
Lookup DescriptorBridge::lookup(uint64_t thread,uintptr_t impl,uintptr_t item,int aux,uint64_t now) const {
    auto it=entries.find({thread,impl});
    if (!impl || it==entries.end()) return {};
    auto const& record=it->second;
    if (now<record.bornMs || now-record.bornMs>LifetimeMs) return {Resolution::Expired,{}};
    if (record.item!=item || record.aux!=aux) return {Resolution::FingerprintMismatch,{}};
    return {Resolution::Candidate,record};
}
void DescriptorBridge::clear() { entries.clear(); }
}
