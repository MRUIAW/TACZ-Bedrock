#pragma once
#include <cstdint>
namespace tacz::client::hitdiag {
void shot(int64_t timestamp, uint64_t inputTick);
void tick();
void clear();
}
