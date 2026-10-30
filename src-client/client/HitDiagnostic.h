#pragma once
#include <cstdint>
namespace tacz::client::hitdiag {
void shot(int64_t timestamp);
void tick();
void clear();
}
