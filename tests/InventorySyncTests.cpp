#include <doctest/doctest.h>
#include <array>
#include <string>
#include <map>
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
TEST_CASE("Server NBT-only writes request native force balance as well as full slot values") {
    struct Inventory {
        int slot=-1;
        Stack value;
        bool balanced=false;
        void setItemWithForceBalance(int s,Stack const& item,bool force) { slot=s; value=item; balanced=force; }
    } inventory;
    Stack gun{1,101,"GunCurrentAmmoCount=26"};
    model::writeServerInventorySlot(inventory,2,gun);
    CHECK(inventory.slot==2); CHECK(inventory.balanced); CHECK(inventory.value==gun);
    model::writeServerInventorySlot(inventory,5,Stack{35,102,"AmmoId=762x39"});
    CHECK(inventory.slot==5); CHECK(inventory.balanced); CHECK(inventory.value.count==35);
}
TEST_CASE("Native balance model: NBT gun groups can block an otherwise balanced pickup") {
    // Model of the measured native grouping rule (item/aux/tag + signed count),
    // not an engine/network integration test. A count-only model misses this bug.
    struct Transaction {
        std::map<std::string,int> groups;
        int sends=0;
        void add(std::string const& key,int delta,bool force=false) {
            groups[key]+=delta;
            if (force) for (auto& [_,balance]:groups) balance=0;
            for (auto const& [_,balance]:groups) if (balance) return;
            ++sends; groups.clear();
        }
    } oldPath;
    oldPath.add("gun/NBT-old",-1); oldPath.add("gun/NBT-new",1);
    oldPath.add("ammo/NBT",25); oldPath.add("ammo/NBT",-25);
    CHECK(oldPath.sends==0); CHECK(oldPath.groups.at("ammo/NBT")==0);
    CHECK(oldPath.groups.at("gun/NBT-old")==-1); CHECK(oldPath.groups.at("gun/NBT-new")==1);
    Transaction fixed;
    fixed.add("gun/NBT-old",-1); fixed.add("gun/NBT-new",1,true);
    CHECK(fixed.sends==1); CHECK(fixed.groups.empty());
    // Full + partial pickup from two actors, as seen in the 0.3.31 log.
    fixed.add("ammo/NBT",24); fixed.add("ammo/NBT",-24);
    CHECK(fixed.sends==2); CHECK(fixed.groups.empty());
    fixed.add("ammo/NBT",1); fixed.add("ammo/NBT",-1);
    CHECK(fixed.sends==3); CHECK(fixed.groups.empty());
    // Later shots/reloads must not alternate success and mismatch recovery.
    for (int i=0;i<8;++i) {
        fixed.add("gun/NBT-old",-1); fixed.add("gun/NBT-new",1,true);
        fixed.add("ammo/NBT",19); fixed.add("ammo/NBT",-19);
        CHECK(fixed.groups.empty()); CHECK(fixed.sends==5+2*i);
    }
}
