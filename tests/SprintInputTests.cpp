#include <doctest/doctest.h>
#include <bitset>
#include "model/SprintInput.h"

using tacz::model::SprintInputGate;
namespace {
// Model the native input producers separately from the policy. Toggle supplies
// its still-active native latch; double-tap supplies a one-shot sprint request,
// NOT a permanent SprintDown key. No guessed Bedrock double-tap timer is emulated.
struct Pipeline {
    SprintInputGate gate;
    bool tick(bool blocked,bool nativeLatch,bool doubleTapRequest=false,bool eligible=true) {
        auto d=gate.sample(blocked,nativeLatch,doubleTapRequest);
        std::bitset<27> raw,processed; raw.set(8,nativeLatch); processed=raw;
        bool extractedSprint=false;
        auto nativeUpdate=[&] { extractedSprint=(raw.test(8)||doubleTapRequest)&&eligible; };
        if (blocked) {
            tacz::model::withSprintMasked(raw,processed,8,nativeUpdate);
            extractedSprint=d.sprint;
        } else nativeUpdate();
        CHECK(raw.test(8)==nativeLatch); CHECK(processed.test(8)==nativeLatch);
        // The later raw-HID and packet stages must not see restored intent as an
        // allowed request. This was the boundary missing in 0.3.16.
        bool rawPrediction=blocked ? false : raw.test(8);
        std::bitset<66> wire;
        wire.set(4,raw.test(8)); wire.set(20,extractedSprint); wire.set(25,extractedSprint);
        wire.set(26); wire.set(10); wire.set(34);
        if (blocked) tacz::model::suppressSprintRequests(wire,4,20,25);
        CHECK(wire.test(26)); CHECK(wire.test(10)); CHECK(wire.test(34));
        if (blocked) {
            CHECK_FALSE(d.down); CHECK_FALSE(d.sprint); CHECK_FALSE(rawPrediction);
            CHECK_FALSE(wire.test(4)); CHECK_FALSE(wire.test(20)); CHECK_FALSE(wire.test(25));
            CHECK_FALSE(extractedSprint);
        }
        return extractedSprint;
    }
};
}

TEST_CASE("Sprint pipeline: held key survives ADS without prediction/network re-entry") {
    Pipeline p;
    CHECK(p.tick(false,true));
    for (int tick=0;tick<120;++tick) CHECK_FALSE(p.tick(true,true));
    CHECK(p.tick(false,true)); CHECK(p.gate.phase==SprintInputGate::Phase::ResumeNative);
    CHECK(p.tick(false,true)); CHECK(p.gate.phase==SprintInputGate::Phase::Open);
    CHECK_FALSE(p.tick(false,false));
}
TEST_CASE("Sprint pipeline: key release in ADS and vanilla ineligibility never force resume") {
    Pipeline p;
    p.tick(true,true); p.tick(true,false);
    CHECK_FALSE(p.tick(false,false));
    p.tick(true,true);
    CHECK_FALSE(p.tick(false,true,false,false)); // hunger/forward/collision eligibility
    CHECK(p.tick(false,true,false,true));
}
TEST_CASE("Sprint pipeline: native toggle stays active or is cancelled while ADS is blocked") {
    Pipeline p;
    CHECK(p.tick(false,true)); // native toggle on; physical button may already be up
    for (int i=0;i<20;++i) CHECK_FALSE(p.tick(true,true));
    CHECK(p.tick(false,true)); // logical native toggle still active
    p.tick(true,true); p.tick(true,false); // native toggle-off callback changes latch
    CHECK_FALSE(p.tick(false,false));
    p.tick(true,true); // toggle on while aiming is intent, not a sprint request
    CHECK(p.tick(false,true));
}
TEST_CASE("Sprint pipeline: double-tap W is suppressed, not queued as a held sprint key") {
    Pipeline p;
    CHECK(p.tick(false,false,true)); // a native W double-tap can start sprint normally
    CHECK_FALSE(p.tick(true,false));
    for (int i=0;i<10;++i) CHECK_FALSE(p.tick(true,false,true));
    CHECK_FALSE(p.tick(false,false)); // no latent double-tap after ADS
    CHECK_FALSE(p.tick(false,false)); // no held-key manufacture
    CHECK(p.tick(false,false,true)); // fresh native double-tap after ADS works
}
TEST_CASE("Sprint regression explanation: clearing intent loses resume; restoring before egress leaks it") {
    std::bitset<27> raw,processed; raw.set(8); processed=raw;
    raw.reset(8); processed.reset(8); // 0.3.15
    CHECK_FALSE(raw.test(8)); // no new down event while the key stays held
    raw.set(8); processed.set(8);
    tacz::model::withSprintMasked(raw,processed,8,[&] { CHECK_FALSE(raw.test(8)); }); // 0.3.16
    CHECK(raw.test(8)); // later RawMoveInput/send reads true again
    std::bitset<66> wire; wire.set(4,raw.test(8)); wire.set(20); wire.set(25);
    tacz::model::suppressSprintRequests(wire,4,20,25); // 0.3.17 egress gate
    CHECK_FALSE(wire.test(4)); CHECK_FALSE(wire.test(20)); CHECK_FALSE(wire.test(25));
    CHECK(raw.test(8)); // independent, live intent still available after ADS
}
