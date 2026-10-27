#include <doctest/doctest.h>
#include <array>
#include <string>
#include "debug/AmmoSyncDebug.h"
#include "model/InventorySlotSync.h"
using namespace tacz;
namespace {
struct Stack {
    int count=0,netId=0;
    std::string nbt;
    bool operator==(Stack const&) const=default;
};
}
TEST_CASE("A sent inventory slot commits count NBT and net ID without touching unsent slots") {
    std::array<Stack,3> baseline{{{60,11,"ammo"},{1,12,"gun-ammo=30"},{60,13,"other"}}};
    auto original=baseline;
    CHECK(model::commitSentInventorySlot(baseline,0,Stack{56,11,"ammo"}));
    CHECK(baseline[0]==Stack{56,11,"ammo"});
    CHECK(baseline[1]==original[1]); CHECK(baseline[2]==original[2]);
    CHECK(model::commitSentInventorySlot(baseline,1,Stack{1,12,"gun-ammo=26"}));
    CHECK(baseline[1].nbt=="gun-ammo=26"); CHECK(baseline[0].count==56);
    CHECK(model::commitSentInventorySlot(baseline,0,Stack{60,21,"ammo"}));
    CHECK(baseline[0].netId==21);
    CHECK(model::commitSentInventorySlot(baseline,0,Stack{})); CHECK(baseline[0]==Stack{});
    auto saved=baseline;
    CHECK_FALSE(model::commitSentInventorySlot(baseline,-1,Stack{99,99,"bad"}));
    CHECK_FALSE(model::commitSentInventorySlot(baseline,3,Stack{99,99,"bad"})); CHECK(baseline==saved);
}
TEST_CASE("Slot commit models consume refill and depletion without hiding a later real count change") {
    for (int used=1;used<=60;++used) {
        std::array<Stack,1> baseline{{{60,11,"ammo"}}};
        Stack consumed{60-used,used==60 ? 0 : 11,used==60 ? "" : "ammo"};
        CHECK(model::commitSentInventorySlot(baseline,0,consumed));
        Stack refilled{60,used==60 ? 22 : 11,"ammo"};
        CHECK(baseline[0]!=refilled); // no stale 60 baseline after a 56 packet
        CHECK(model::commitSentInventorySlot(baseline,0,refilled)); CHECK(baseline[0]==refilled);
    }
}
TEST_CASE("Ammo synchronization diagnostics are off by default and capped on every rearm") {
    debug::AmmoSyncBudget budget;
    CHECK_FALSE(budget.take());
    for (int run=0;run<3;++run) {
        budget.arm(true);
        for (int i=0;i<256;++i) CHECK(budget.take());
        for (int i=0;i<512;++i) CHECK_FALSE(budget.take());
    }
    budget.arm(true); CHECK(budget.take()); budget.arm(false); CHECK_FALSE(budget.take());
}
