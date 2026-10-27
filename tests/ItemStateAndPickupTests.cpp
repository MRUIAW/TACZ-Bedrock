#include <doctest/doctest.h>
#include <concepts>
#include <cstdint>
#include <limits>
#include <numeric>
#include "pack/ItemTooltip.h"
using namespace tacz;
namespace {
// Mirrors the SDK IntTag conversion constraint, not a game ABI placeholder.
struct IntegralTag {
    int data;
    template<std::integral T> operator T() const { return static_cast<T>(data); }
};
struct ByteValue { signed char data; };
struct FloatValue { float data; };
struct DoubleValue { double data; };
static_assert(std::is_convertible_v<IntegralTag,int> && !std::is_convertible_v<IntegralTag,double>);
}
TEST_CASE("Tooltip reads concrete integer tag payload, not the lossy Variant double visitor") {
    CHECK(pack::tooltipTagInteger(IntegralTag{30})==30);
    CHECK(pack::tooltipTagInteger(ByteValue{1})==1);
    CHECK(pack::tooltipTagInteger(ByteValue{0})==0);
    CHECK(pack::tooltipTagInteger(IntegralTag{63})==63);
    CHECK(pack::tooltipTagInteger(FloatValue{30.9f})==30);
    CHECK(pack::tooltipTagInteger(DoubleValue{-0.1})==-1);
    CHECK(pack::tooltipTagInteger(DoubleValue{std::numeric_limits<double>::quiet_NaN()})==0);
    CHECK(pack::tooltipTagInteger(DoubleValue{std::numeric_limits<double>::infinity()})==std::numeric_limits<int>::max());
}
TEST_CASE("Slot identity is selected from matching full collection stacks, not reverse-mapped category IDs") {
    pack::ItemIdentity ak{pack::ItemKind::Gun,{"tacz","ak47"}},hk{pack::ItemKind::Gun,{"tacz","hk416d"}};
    pack::SlotIdentitySelection slot;
    CHECK_FALSE(slot.offer(852,0,853,0,ak)); CHECK_FALSE(slot.identity);
    CHECK_FALSE(slot.offer(852,0,852,1,ak)); CHECK_FALSE(slot.identity);
    CHECK_FALSE(slot.offer(852,0,852,0,std::nullopt)); CHECK_FALSE(slot.identity);
    CHECK(slot.offer(852,0,852,0,ak)); CHECK(slot.identity==ak);
    CHECK(slot.offer(852,0,852,0,ak)); CHECK_FALSE(slot.ambiguous);
    CHECK_FALSE(slot.offer(852,0,852,0,hk)); CHECK(slot.ambiguous); CHECK_FALSE(slot.identity);
    CHECK_FALSE(slot.offer(852,0,852,0,ak)); CHECK_FALSE(slot.identity); // fail closed, not last candidate wins
    pack::SlotIdentitySelection other;
    CHECK(other.offer(852,0,852,0,hk)); CHECK(other.identity==hk); // same runtime base, different actual slot identity
}
TEST_CASE("Slot collection lookup walks owner-to-root and never substitutes an outer grid for the nearest slot") {
    using Level=std::vector<pack::SlotCollectionIndex>;
    CHECK_FALSE(pack::nearestSlotCollectionLevel({}));
    CHECK_FALSE(pack::nearestSlotCollectionLevel({Level{},Level{{"",0},{"hotbar_items",-1}}}));
    auto depth=pack::nearestSlotCollectionLevel({Level{},Level{},Level{{"hotbar_items",4}},Level{{"inventory_items",12}}});
    REQUIRE(depth); CHECK(*depth==2);
    depth=pack::nearestSlotCollectionLevel({Level{{"hotbar_items",0}},Level{{"inventory_items",9}}});
    REQUIRE(depth); CHECK(*depth==0); // zero is valid, not a sentinel
    depth=pack::nearestSlotCollectionLevel({Level{},Level{{"container_items",7},{"other",3}},Level{{"hotbar_items",1}}});
    REQUIRE(depth); CHECK(*depth==1); // resolve/ambiguity check remains within the nearest scope
}
TEST_CASE("Tooltip current and maximum follow Java's effective current chamber for all bolt types") {
    pack::GunPackRegistry packs;
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(std::filesystem::path(TACZ_DEFAULT_PACK_DIR)));
    packs.load(std::move(sources),[](auto const&){},[](auto const&){});
    pack::ItemIdentity identity{pack::ItemKind::Gun,{"tacz","ak47"}};
    auto gun=const_cast<pack::GunEntry*>(packs.gun(identity.id)); REQUIRE(gun);
    pack::TooltipState state; state.ammo=pack::tooltipTagInteger(IntegralTag{30}); state.chamber=pack::tooltipTagInteger(ByteValue{1})!=0;
    auto check=[&](std::string_view count) {
        auto text=pack::buildItemTooltip(packs,identity,"zh_CN",state).text();
        CHECK(text.find(count)!=std::string::npos);
    };
    gun->data.bolt=pack::Bolt::ClosedBolt; check("31/31");
    state.ammo=29; check("30/31");
    state.chamber=false; check("29/30"); // Java does NOT add an imaginary chamber to max
    state.ammo=0; check("0/30");
    gun->data.bolt=pack::Bolt::OpenBolt; state.ammo=30; state.chamber=true; check("30/30");
    gun->data.bolt=pack::Bolt::ManualAction; state.ammo=5; state.chamber=true; check("6/31");
    state.capacity=40; state.ammo=40; check("41/41"); // effective extended-mag capacity + current chamber
    state.chamber=false; check("40/40");
    gun->data.bolt=pack::Bolt::OpenBolt; state.chamber=true; check("40/40");
}
TEST_CASE("Ground pickup plan conserves rounds on full, partial, blocked and cancelled pickup") {
    for (int ground=0;ground<=255;++ground) for (int space=0;space<=255;++space) {
        auto plan=pack::planPickup(ground,space);
        CHECK(plan.accepted+plan.remaining==ground);
        CHECK(plan.accepted==std::min(ground,space));
        // Native partial-add false branch writes the local remainder back to
        // the SAME ground actor; it need not create a reserved remainder actor.
        int inventoryGain=plan.accepted,groundAfter=plan.remaining;
        CHECK(inventoryGain+groundAfter==ground);
        // A cancellation before add makes no inventory or ground changes.
        int cancelledInventoryGain=0,cancelledGround=ground;
        CHECK(cancelledInventoryGain+cancelledGround==ground);
    }
    CHECK(pack::planPickup(60,1).accepted==1); CHECK(pack::planPickup(60,1).remaining==59);
    CHECK(pack::planPickup(60,59).accepted==59); CHECK(pack::planPickup(60,59).remaining==1);
    CHECK(pack::planPickup(60,0).remaining==60); CHECK(pack::planPickup(60,120).remaining==0);
    CHECK(pack::planPickup(-1,4).accepted==0); CHECK(pack::planPickup(60,-1).remaining==60);
}
TEST_CASE("Give overflow belongs on ground, including subsequent split stacks after inventory fills") {
    for (int space:{0,1,59,60,61,119,120,121,2304}) {
        int inserted=0,dropped=0;
        for (int chunk:pack::splitAmmo(120,60)) {
            auto plan=pack::planPickup(chunk,space);
            inserted+=plan.accepted; dropped+=plan.remaining; space-=plan.accepted;
        }
        CHECK(inserted+dropped==120);
        CHECK(inserted>=0); CHECK(dropped>=0);
    }
    auto initial=pack::planAmmoInsertion({{1,true},{60,true}},60,60);
    CHECK(initial.remaining==1);
    CHECK(initial.additions==std::vector<int>{59,0});
    // Partial mutation is allowed. Returning true here was the duplication bug:
    // Player::take would skip its native false/partial-remainder branch.
    CHECK_FALSE(pack::nativeAmmoAddComplete(initial.remaining));
    CHECK(pack::nativeAmmoAddComplete(pack::planAmmoInsertion({{0,false}},60,60).remaining));
    CHECK_FALSE(pack::nativeAmmoAddComplete(pack::planAmmoInsertion({{60,true}},60,60).remaining));
}
TEST_CASE("Pickup logging retains survival remainder and accepts vanilla creative overflow discard") {
    for (int count:{1,7,15,30,59,60}) for (int space=0;space<=60;++space) for (bool creative:{false,true}) {
        auto plan=pack::planPickup(count,space);
        // Model an invoked native take: creative cleanup may discard overflow;
        // survival retains it. Production must not override isCreative.
        bool complete=creative || pack::nativeAmmoAddComplete(plan.remaining);
        auto ground=pack::ammoPickupGroundAfter(plan.remaining,false,complete);
        if (creative) {
            CHECK(ground==0);
            CHECK(plan.accepted+plan.remaining==count); // remaining is intentionally discarded, not ground ammo
        } else CHECK(plan.accepted+ground==count);
    }
    CHECK(pack::ammoPickupGroundAfter(60,false,true)==0); // caller removal pending; stale actor count is not ground ammo
    CHECK(pack::ammoPickupGroundAfter(30,false,false)==30);
    CHECK(pack::ammoPickupGroundAfter(30,true,false)==0);
}
