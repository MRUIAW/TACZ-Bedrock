#include <doctest/doctest.h>
#include "pack/DescriptorProbe.h"
using namespace tacz::pack::probe;
namespace {
DescriptorRecord ammo(uint64_t born=1000) { return {1234,0,60,"tacz:762x39",born,0x25a4822,false}; }
}
TEST_CASE("Descriptor bridge keys allocation identity plus thread, not descriptor wrapper or ItemId") {
    DescriptorBridge bridge;
    bridge.put(10,200,ammo());
    auto hit=bridge.lookup(10,200,1234,0,1001);
    REQUIRE(hit.record); CHECK(hit.record->ammo=="tacz:762x39"); CHECK(hit.record->limit==60);
    CHECK(bridge.lookup(11,200,1234,0,1001).status==Resolution::Missing);
    CHECK(bridge.lookup(10,201,1234,0,1001).status==Resolution::Missing);
    CHECK(bridge.lookup(10,200,4321,0,1001).status==Resolution::FingerprintMismatch);
    CHECK(bridge.lookup(10,200,1234,1,1001).status==Resolution::FingerprintMismatch);
    // Moving a descriptor wrapper keeps mImpl and therefore this key unchanged.
    CHECK(bridge.lookup(10,200,1234,0,1002).status==Resolution::Candidate);
    bridge.erase(10,200); // observed raw descriptor constructor invalidates reuse
    CHECK(bridge.lookup(10,200,1234,0,1003).status==Resolution::Missing);
    auto other=ammo(); other.ammo="tacz:9mm"; other.limit=48;
    bridge.put(10,200,other);
    CHECK(bridge.lookup(10,200,1234,0,1004).record->ammo=="tacz:9mm");
}
TEST_CASE("Probe copies retain original expiry and world cleanup clears all candidates") {
    DescriptorBridge bridge; bridge.put(10,200,ammo());
    auto hit=bridge.lookup(10,200,1234,0,1200); REQUIRE(hit.record);
    hit.record->copied=true; bridge.put(10,300,*hit.record);
    REQUIRE(bridge.lookup(10,300,1234,0,1250).record);
    CHECK(bridge.lookup(10,300,1234,0,1250).record->copied);
    CHECK(bridge.lookup(10,300,1234,0,1251).status==Resolution::Expired);
    CHECK(bridge.lookup(10,200,1234,0,999).status==Resolution::Expired);
    CHECK(bridge.copied==1);
    bridge.clear(); CHECK(bridge.size()==0); CHECK(bridge.published==0);
    CHECK(bridge.lookup(10,200,1234,0,1001).status==Resolution::Missing);
}
TEST_CASE("Measurement bridge is bounded and never interprets cached pointers as native objects") {
    DescriptorBridge bridge;
    bridge.put(1,0,ammo()); CHECK(bridge.size()==0);
    for (uintptr_t i=1;i<=DescriptorBridge::Capacity+1;++i) bridge.put(1,i,ammo());
    CHECK(bridge.size()==1); CHECK(bridge.capacityCleared==DescriptorBridge::Capacity);
    CHECK(bridge.lookup(1,1,1234,0,1001).status==Resolution::Missing);
}
TEST_CASE("Probe aggregates actual native values and hypothetical limits by caller and thread") {
    Samples samples;
    Caller site{0x25a482f,10,true,0};
    Lookup hit{Resolution::Candidate,ammo()};
    CHECK(samples.add(site,64,hit)); CHECK(samples.add(site,64,{}));
    hit.record->copied=true;
    CHECK(samples.add(site,60,hit));
    auto const& total=samples.total.at(site);
    CHECK(total.calls==3); CHECK(total.resolved==2); CHECK(total.different==1); CHECK(total.copyHits==1);
    CHECK(total.native.at(64)==2); CHECK(total.native.at(60)==1); CHECK(total.would.at(60)==2);
    auto window=samples.takeWindow(); CHECK(window.at(site).calls==3); CHECK(samples.window.empty());
    CHECK(samples.total.at(site).calls==3);
    auto server=site; server.thread=11; CHECK(samples.add(server,64,hit)); CHECK(samples.total.size()==2);
    auto external=site; external.nativeImage=false; CHECK(samples.add(external,64,{})); CHECK(samples.total.size()==3);
    samples.clear(); CHECK(samples.total.empty()); CHECK(samples.window.empty());
}
TEST_CASE("Probe caller aggregation cap counts overflow without growing indefinitely") {
    Samples samples;
    for (uintptr_t i=0;i<Samples::Capacity;++i) samples.add({i,1,true,0},64,{});
    CHECK_FALSE(samples.add({999999,1,true,0},64,{})); CHECK(samples.dropped==1);
    CHECK(samples.total.size()==Samples::Capacity);
    CHECK(samples.add({0,1,true,0},64,{})); CHECK(samples.total.at({0,1,true,0}).calls==2);
}
