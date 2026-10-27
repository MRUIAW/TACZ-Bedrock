#include "pack/DescriptorProbe.h"
#include <utility>
namespace tacz::pack::probe {
char const* resolutionName(Resolution r) {
    switch(r) {
    case Resolution::Candidate:return "candidate";
    case Resolution::Expired:return "expired";
    case Resolution::FingerprintMismatch:return "fingerprint-mismatch";
    default:return "missing";
    }
}
void DescriptorBridge::erase(uint64_t thread,uintptr_t impl) { invalidated+=entries.erase({thread,impl}); }
void DescriptorBridge::put(uint64_t thread,uintptr_t impl,DescriptorRecord record) {
    if (!impl) return;
    erase(thread,impl);
    if (entries.size()>=Capacity) { capacityCleared+=entries.size(); entries.clear(); }
    ++published; if (record.copied) ++copied;
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
void DescriptorBridge::clear() { entries.clear(); invalidated=capacityCleared=published=copied=0; }
void Counts::add(int value,Lookup const& hit) {
    ++calls; ++native[value]; ++status[resolutionName(hit.status)];
    if (hit.record) {
        ++resolved; ++would[hit.record->limit]; different+=value!=hit.record->limit;
        copyHits+=hit.record->copied;
        if (ammo.contains(hit.record->ammo) || ammo.size()<8) ++ammo[hit.record->ammo];
        else ++ammo["<other>"];
    }
}
bool Samples::add(Caller caller,int value,Lookup const& hit) {
    if (!total.contains(caller) && total.size()>=Capacity) { ++dropped; return false; }
    total[caller].add(value,hit); window[caller].add(value,hit); return true;
}
std::map<Caller,Counts> Samples::takeWindow() { return std::exchange(window,{}); }
void Samples::clear() { window.clear(); total.clear(); dropped=0; }
}
