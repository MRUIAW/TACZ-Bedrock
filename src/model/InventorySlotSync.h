#pragma once
#include <cstddef>
namespace tacz::model {
// A sent slot commits a complete value, never just its count and never a whole
// container snapshot. Native ItemStack assignment also carries NBT and net ID.
template<class Slots,class Stack>
bool commitSentInventorySlot(Slots& baseline,int slot,Stack const& sent) {
    if (slot<0 || static_cast<std::size_t>(slot)>=baseline.size()) return false;
    baseline[slot]=sent;
    return true;
}
}
