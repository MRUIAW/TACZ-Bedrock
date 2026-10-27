#include <doctest/doctest.h>
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
TEST_CASE("PNG alpha is inspected without dropping or changing zero and partial alpha") {
    uint8_t rgba[]={30,40,50,0,60,70,80,128,90,100,110,255};
    auto a=model::iconAlpha(rgba); CHECK(a.valid); CHECK(a.transparent==1); CHECK(a.partial==1); CHECK(a.opaque==1);
    CHECK(rgba[3]==0); CHECK(rgba[7]==128); CHECK(rgba[11]==255);
    CHECK_FALSE(model::iconAlpha({}).valid);
    CHECK_FALSE(model::iconAlpha(std::span<uint8_t const>(rgba,3)).valid);
    CHECK(model::SlotIconMaterial=="ui_textured_and_glcolor_sprite");
}
TEST_CASE("Only cursor-specific UI controls may borrow the authoritative cursor stack") {
    CHECK(model::cursorIconControl("selected_item_icon"));
    CHECK(model::cursorIconControl("common.inventory_selected_icon"));
    CHECK(model::cursorIconControl("inventory_selected_icon_button"));
    CHECK_FALSE(model::cursorIconControl("item_icon"));
    CHECK_FALSE(model::cursorIconControl("grid_item_for_inventory"));
    CHECK_FALSE(model::cursorIconControl("hover_text"));
}
