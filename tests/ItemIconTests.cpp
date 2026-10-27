#include <doctest/doctest.h>
#include <bit>
#include <cstdint>
#include <limits>
#include "model/ItemIcon.h"
#include "pack/PackItems.h"
using namespace tacz;
TEST_CASE("Slot identity remains usable on repeated render frames without a fresh update") {
    pack::SlotIconBinding binding{3,100,852,0,pack::ItemIdentity{pack::ItemKind::Ammo,{"tacz","762x39"}}};
    for (int frame=0;frame<180;++frame) CHECK(binding.matches(3,100,852,0));
    CHECK_FALSE(binding.matches(0,100,852,0)); CHECK_FALSE(binding.matches(4,100,852,0));
    CHECK_FALSE(binding.matches(3,101,852,0)); CHECK_FALSE(binding.matches(3,100,854,0));
    CHECK_FALSE(binding.matches(3,100,852,1));
    binding.owner=101; CHECK(binding.matches(3,101,852,0));
    auto clone=binding; clone.owner=0;
    CHECK(clone.matches(3,0,852,0)); // may request custom batch, not borrow another control's binding
    CHECK_FALSE(clone.matches(3,101,852,0)); // must resolve its actual owner before painting
    binding.identity.reset(); CHECK_FALSE(binding.matches(3,101,852,0));
}
TEST_CASE("Only cursor-specific UI controls may borrow the authoritative cursor stack") {
    CHECK(model::cursorIconControl("selected_item_icon"));
    CHECK(model::cursorIconControl("common.inventory_selected_icon"));
    CHECK(model::cursorIconControl("inventory_selected_icon_button"));
    CHECK_FALSE(model::cursorIconControl("item_icon"));
    CHECK_FALSE(model::cursorIconControl("grid_item_for_inventory"));
    CHECK_FALSE(model::cursorIconControl("hover_text"));
}
TEST_CASE("Flying icons use native eased slot scale without normalizing or adding an alpha curve") {
    CHECK(model::flyingIconSize(275.49625f,230.58562f,1.0f)==16.0f);
    CHECK(model::flyingIconSize(0,0,1.125f)==18.0f);
    CHECK(model::flyingIconSize(-8,4,0.5f)==8.0f);
    CHECK(model::flyingIconSize(0,0,0.1f)==1.6f);
    CHECK(model::flyingIconSize(0,0,16)==256.0f);
    // The native renderer supplies the quintic-eased result. Preserve every
    // incoming intermediate scale; this is not an engine animation integration test.
    for (int i=0;i<=100;++i) {
        float t=i/100.0f,u=t-1,eased=1+u*u*u*u*u;
        float scale=1.0f+(1.125f-1.0f)*eased;
        CHECK(model::flyingIconSize(100+eased*40,50-eased*20,scale)==16*scale);
    }
}
TEST_CASE("Unusable flying arguments select native fallback, including zOrder bits mistaken for scale") {
    auto nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
    for (float scale:{0.0f,-1.0f,0.099f,16.001f,nan,inf,-inf,
        std::numeric_limits<float>::denorm_min(),std::bit_cast<float>(uint32_t{23})})
        CHECK_FALSE(model::flyingIconSize(275.49625f,230.58562f,scale));
    CHECK_FALSE(model::flyingIconSize(nan,0,1));
    CHECK_FALSE(model::flyingIconSize(0,nan,1));
    CHECK_FALSE(model::flyingIconSize(inf,0,1));
    CHECK_FALSE(model::flyingIconSize(0,-inf,1));
}
