#include <doctest/doctest.h>
#include <algorithm>

#include "gun/AttachmentRules.h"
#include "lua/GunLogic.h"
#include "lua/AnimationStateMachine.h"
#include "pack/GunPackParser.h"

using namespace tacz;
namespace {
using json = nlohmann::json;

// Synthetic typed NBT-like view: objects are compounds, strings are tag type 8.
gun::NbtReader view(json const& item) {
    return [&item](std::vector<std::string> const& path, std::string const& key, int mode) -> json {
        auto node = &item;
        for (auto const& part : path) {
            if (!node->is_object()) return {};
            auto it = node->find(part);
            if (it == node->end() || !it->is_object()) return {};
            node = &*it;
        }
        if (!node->is_object()) return {};
        auto it = node->find(key);
        if (it == node->end() || it->is_null()) return {};
        if (mode != 0) return *it;
        if (it->is_object()) return 10;
        if (it->is_string()) return 8;
        if (it->is_array()) return 9;
        if (it->is_boolean()) return 1;
        if (it->is_number_integer()) return 3;
        if (it->is_number()) return 6;
        return {};
    };
}

json attached(std::string const& type = "SCOPE", json id = "fixture:lens") {
    return {{"Attachment" + type, {{"tag", {{"AttachmentId", std::move(id)}}}}}};
}

class ScriptSource final : public pack::PackSource {
public:
    std::string label = "attachment fixture";
    std::map<std::string, std::string> files;
    std::string const& name() const override { return label; }
    std::vector<std::string> list(std::string_view prefix) const override {
        std::vector<std::string> out;
        for (auto const& [path, _] : files) if (path.starts_with(prefix)) out.push_back(path);
        return out;
    }
    std::optional<std::string> read(std::string_view path) const override {
        if (path == "gunpack.meta.json") return R"({"namespace":"fixture"})";
        auto it = files.find(std::string(path));
        return it == files.end() ? std::nullopt : std::optional{it->second};
    }
};

pack::GunPackRegistry registry(std::string const& side, std::string const& source) {
    auto pack = std::make_unique<ScriptSource>();
    pack->files[side + "/fixture/scripts/attachment.lua"] = source;
    std::vector<std::unique_ptr<pack::PackSource>> sources; sources.push_back(std::move(pack));
    pack::GunPackRegistry result; result.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    return result;
}
}

TEST_CASE("Attachment lookup requires compounds tag string id and ignores sibling item-stack metadata") {
    gun::AllowedAttachmentTypes allowed{"scope"};
    for (auto item : {
        json{}, json::object(), json{{"AttachmentSCOPE", json::object()}},
        json{{"AttachmentSCOPE", {{"AttachmentId", "fixture:wrong_level"}}}},
        json{{"AttachmentSCOPE", {{"tag", json::object()}}}},
        json{{"AttachmentSCOPE", 42}}, json{{"AttachmentSCOPE", {{"tag", "wrong type"}}}}}) {
        CHECK(gun::getAttachment(&allowed, view(item), "SCOPE") == "tacz:empty");
    }
    for (auto value : {json{}, json(42), json(false), json(1.5), json::array(), json::object()}) {
        auto item = attached("SCOPE", value);
        CHECK(gun::getAttachment(&allowed, view(item), "SCOPE") == "tacz:empty");
    }
    auto item = attached(); auto original = item;
    item["AttachmentSCOPE"]["id"] = "fixture:unrelated_item";
    item["AttachmentSCOPE"]["Count"] = 0;
    CHECK(gun::getAttachment(&allowed, view(item), "SCOPE") == "fixture:lens");
    item.erase("AttachmentSCOPE"); item["attachmentscope"] = original["AttachmentSCOPE"];
    CHECK(gun::getAttachment(&allowed, view(item), "SCOPE") == "tacz:empty");
}

TEST_CASE("Attachment API types are exact constants while allow-list names follow Gson enum aliases") {
    for (auto const& names : gun::AttachmentTypes) {
        auto item = attached(std::string(names.constant));
        for (auto raw : {names.serialized, names.constant}) {
            gun::AllowedAttachmentTypes allowed{std::string(raw)};
            CHECK(gun::getAttachment(&allowed, view(item), names.constant) == "fixture:lens");
        }
    }
    auto item = attached();
    gun::AllowedAttachmentTypes allowed{"scope"};
    for (auto type : {"scope", "Scope", " SCOPE", "SCOPE ", "", "UNKNOWN", "none"}) {
        CHECK(gun::getAttachment(&allowed, view(item), type) == "tacz:empty");
    }
    for (auto raw : {"Scope", " scope", "scope ", "unknown", "none"}) {
        allowed = {raw};
        CHECK(gun::getAttachment(&allowed, view(item), "SCOPE") == "tacz:empty");
    }
    item = attached("NONE"); allowed = {"NONE"};
    CHECK(gun::getAttachment(&allowed, view(item), "NONE") == "fixture:lens");
    allowed = {"none"}; CHECK(gun::getAttachment(&allowed, view(item), "NONE") == "tacz:empty");
}

TEST_CASE("Attachment eligibility rejects unknown data and unlisted types before touching NBT") {
    int reads = 0;
    gun::NbtReader reader = [&](auto const&, auto const&, int) -> json { ++reads; return 10; };
    gun::AllowedAttachmentTypes allowed{"muzzle", std::nullopt, "unknown"};
    CHECK(gun::getAttachment(nullptr, reader, "SCOPE") == "tacz:empty");
    CHECK(gun::getAttachment(&allowed, reader, "SCOPE") == "tacz:empty");
    CHECK(gun::getAttachment(&allowed, reader, "scope") == "tacz:empty");
    allowed.clear(); CHECK(gun::getAttachment(&allowed, reader, "SCOPE") == "tacz:empty");
    CHECK(reads == 0);
    allowed = {"scope"}; CHECK(gun::getAttachment(&allowed, {}, "SCOPE") == "tacz:empty");
}

TEST_CASE("Attachment NBT ResourceLocation parsing matches Java including empty namespace and path") {
    gun::AllowedAttachmentTypes allowed{"scope"};
    for (auto [input, expected] : std::initializer_list<std::pair<std::string, std::string>>{
        {"fixture:lens", "fixture:lens"}, {"lens", "minecraft:lens"}, {":lens", "minecraft:lens"},
        {"", "minecraft:"}, {":", "minecraft:"}, {"fixture:", "fixture:"},
        {"a_0.-:part/a_0.-", "a_0.-:part/a_0.-"}, {"/", "minecraft:/"},
        {"Upper:lens", "tacz:empty"}, {"fixture:Lens", "tacz:empty"},
        {"fixture:lens:other", "tacz:empty"}, {"a/b:lens", "tacz:empty"},
        {" fixture:lens", "tacz:empty"}, {"fixture:lens ", "tacz:empty"},
        {"fixture:bad\\path", "tacz:empty"}, {"fixture:bad#path", "tacz:empty"},
        {std::string("fixture:x\0y", 11), "tacz:empty"}, {"fixture:\xc3\xa9", "tacz:empty"}}) {
        CAPTURE(input);
        auto item = attached("SCOPE", input);
        CHECK(gun::getAttachment(&allowed, view(item), "SCOPE") == expected);
    }
    // This task must not widen the existing pack-resource parser.
    CHECK_FALSE(pack::ResourceId::parse("")); CHECK_FALSE(pack::ResourceId::parse(":lens"));
    CHECK_FALSE(pack::ResourceId::parse("fixture:"));
}

TEST_CASE("Attachment parser preserves absent null unknown entries and numeric enum nulls") {
    auto item = attached();
    for (auto text : {"{}", R"({"allow_attachment_types":null})", R"({"allow_attachment_types":[]})",
                      R"({"allow_attachment_types":[null,"unknown","Scope"]})"}) {
        auto data = pack::parseGunData(pack::parseJsonText(text));
        CHECK(gun::getAttachment(&data.allowAttachmentTypes, view(item), "SCOPE") == "tacz:empty");
    }
    auto data = pack::parseGunData(pack::parseJsonText(R"({"allow_attachment_types":["scope",42,-1,1.5,1e3,null,"unknown"]})"));
    REQUIRE(data.allowAttachmentTypes.size() == 7);
    CHECK(data.allowAttachmentTypes[0] == "scope");
    for (size_t i = 1; i <= 5; ++i) CHECK_FALSE(data.allowAttachmentTypes[i]);
    CHECK(data.allowAttachmentTypes[6] == "unknown"); // mapped to no enum by the shared helper
    CHECK(gun::getAttachment(&data.allowAttachmentTypes, view(item), "SCOPE") == "fixture:lens");
}

TEST_CASE("Attachment parser still rejects booleans objects arrays and non-array lists without widening other fields") {
    for (auto text : {R"({"allow_attachment_types":[true]})", R"({"allow_attachment_types":[false]})",
                      R"({"allow_attachment_types":[{}]})", R"({"allow_attachment_types":[[]]})",
                      R"({"allow_attachment_types":42})", R"({"allow_attachment_types":true})",
                      R"({"allow_attachment_types":"scope"})", R"({"allow_attachment_types":{}})"}) {
        CHECK_THROWS_AS(pack::parseGunData(pack::parseJsonText(text)), pack::ParseError);
    }
    CHECK_THROWS_AS(pack::parseGunData(pack::parseJsonText(R"({"fire_mode":[42]})")), pack::ParseError);
    CHECK(pack::parseGunIndex(pack::parseJsonText(R"({"name":42})")).name == "42");
    CHECK(pack::parseGunIndex(pack::parseJsonText(R"({"name":true})")).name == "true");
}

TEST_CASE("Attachment lookup never substitutes builtin attachments or validates attachment registry existence") {
    auto data = pack::parseGunData(pack::parseJsonText(R"({"allow_attachment_types":["scope"],"builtin_attachments":{"scope":"fixture:builtin"}})"));
    lua::ScriptApi api; api.data = &data;
    CHECK(api.getAttachment("SCOPE") == "tacz:empty");
    auto item = attached("SCOPE", "fixture:not_registered"); api.readNbt = view(item);
    CHECK(api.getAttachment("SCOPE") == "fixture:not_registered");
    data.allowAttachmentTypes.clear(); CHECK(api.getAttachment("SCOPE") == "tacz:empty");
    api.data = nullptr; CHECK(api.getAttachment("SCOPE") == "tacz:empty");
}

TEST_CASE("Gun logic getAttachment uses the shared eligibility and item view without writing NBT") {
    auto packs = registry("data", R"(return {start_bolt=function(api)
        assert(api:getAttachment('SCOPE') == api:getScriptParams().expected)
        assert(api:getAttachment('scope') == 'tacz:empty')
        assert(api:getAttachment('MUZZLE') == 'tacz:empty')
        assert(api:getAttachment('NONE') == 'tacz:empty')
        return true
    end})");
    std::vector<std::string> warnings;
    lua::GunLogic logic(packs, [&](auto const& w) { warnings.push_back(w); });
    auto item = attached(); auto original = item;
    for (bool permitted : {false, true}) {
        auto data = pack::parseGunData(pack::parseJsonText(R"({"script":"fixture:attachment"})"));
        if (permitted) data.allowAttachmentTypes = {"scope"};
        data.scriptParam = {{"expected", permitted ? "fixture:lens" : "tacz:empty"}};
        lua::ScriptApi api; api.data = &data; api.gunId = {"fixture", permitted ? "allowed" : "disallowed"};
        api.readNbt = view(item);
        CHECK(logic.startBolt(api));
    }
    CHECK(item == original); CHECK(warnings.empty());
}

TEST_CASE("Animation getAttachment uses its own gun data and refreshed NBT view including unknown data") {
    auto packs = registry("assets", R"(
        local state = {update=function(self,c)
            c:setShouldHideCrossHair(c:getAttachment('SCOPE') == 'fixture:lens')
            assert(c:getAttachment('scope') == 'tacz:empty')
        end}
        return {states=function() return {state} end}
    )");
    animation::AnimationFile clips;
    lua::AnimationContext context;
    auto item = attached(); auto original = item; context.readNbt = view(item);
    gun::AllowedAttachmentTypes clientAllowed{"scope"}, otherAllowed{"muzzle"};
    std::vector<std::string> warnings;
    lua::AnimationStateMachine machine(packs, clips, {"fixture", "attachment"}, context, [&](auto const& w) { warnings.push_back(w); });
    REQUIRE(machine.initialize(0));
    machine.update(.1); CHECK_FALSE(context.hideCrossHair); // unknown gun data
    context.allowedAttachmentTypes = &clientAllowed;
    machine.update(.2); CHECK(context.hideCrossHair);
    context.allowedAttachmentTypes = &otherAllowed;
    machine.update(.3); CHECK_FALSE(context.hideCrossHair); // same NBT, this side's disallowed list
    context.allowedAttachmentTypes = &clientAllowed;
    item["AttachmentSCOPE"]["tag"]["AttachmentId"] = "fixture:changed";
    machine.update(.4); CHECK_FALSE(context.hideCrossHair);
    item = original; machine.update(.5); CHECK(context.hideCrossHair);
    CHECK(machine.healthy()); CHECK(warnings.empty()); CHECK(item == original);
}

TEST_CASE("Gun logic Java boolean parameters use LuaJ truthiness without relaxing hook return checks") {
    auto packs = registry("data", R"(
        local function exercise(api)
            local cases = {
                {value=0, expected=true}, {value=1, expected=true},
                {value='text', expected=true}, {value={}, expected=true},
                {value=nil, expected=false}, {value=false, expected=false}, {value=true, expected=true}
            }
            for _, case in ipairs(cases) do
                api:setAmmoInBarrel(case.value)
                assert(api:hasAmmoInBarrel() == case.expected)
            end
        end
        return {
            start_bolt=function(api) exercise(api); return 1 end,
            tick_bolt=function(api) exercise(api); return 'true' end,
            start_reload=function(api) exercise(api); return {} end
        }
    )");
    auto data = pack::parseGunData(pack::parseJsonText(R"({"bolt":"manual_action","script":"fixture:attachment"})"));
    lua::ScriptApi api; api.data = &data; api.gunId = {"fixture", "boolean_parameters"};
    std::vector<std::string> warnings;
    lua::GunLogic logic(packs, [&](auto const& w) { warnings.push_back(w); });
    CHECK_FALSE(logic.startBolt(api)); CHECK(api.ammo.barrel);
    CHECK_FALSE(logic.tickBolt(api)); CHECK(api.ammo.barrel);
    CHECK_FALSE(logic.startReload(api)); CHECK(api.ammo.barrel);
    REQUIRE(warnings.size() == 2); // strict bolt checkboolean failures, not argument failures
    for (auto const& warning : warnings) CHECK(warning.find("boolean result expected") != std::string::npos);
}

TEST_CASE("Synthetic empty clip-load reload preserves LuaJ zero-stock chamber truthiness") {
    auto packs = registry("data", R"(
        return {
            start_reload=function(api) api:cacheScriptData({loaded=false}); return true end,
            tick_reload=function(api)
                assert(api:getAttachment('SCOPE') == 'tacz:empty')
                local time = api:getReloadTime()
                local params = api:getScriptParams()
                if time < params.feed * 1000 then return EMPTY_RELOAD_FEEDING, params.feed * 1000 - time end
                local cache = api:getCachedScriptData()
                if not cache.loaded then
                    api:setAmmoInBarrel(api:consumeAmmoFromPlayer(1))
                    api:putAmmoInMagazine(api:consumeAmmoFromPlayer(api:getNeededAmmoAmount()))
                    cache.loaded = true
                end
                if time < params.finish * 1000 then return EMPTY_RELOAD_FINISHING, params.finish * 1000 - time end
                return NOT_RELOADING, -1
            end
        }
    )");
    for (int initialStock : {0, 1, 6}) {
        CAPTURE(initialStock);
        auto data = pack::parseGunData(pack::parseJsonText(R"({"bolt":"manual_action","script":"fixture:attachment","allow_attachment_types":["scope"],"script_param":{"feed":2.8,"finish":3.0}})"));
        lua::ScriptApi api; api.data = &data; api.gunId = {"fixture", "clip_load"}; api.maxAmmo = 5;
        api.reloadState = gun::ReloadStateType::EmptyFeeding;
        int stock = initialStock, calls = 0;
        api.extractAmmo = [&](int amount) { ++calls; int taken = std::min(stock, amount); stock -= taken; return taken; };
        std::vector<std::string> warnings;
        lua::GunLogic logic(packs, [&](auto const& w) { warnings.push_back(w); });
        REQUIRE(logic.startReload(api));
        api.reloadTimeMs = 2799;
        CHECK(logic.tickReload(api).type == gun::ReloadStateType::EmptyFeeding); CHECK_FALSE(api.ammo.barrel);
        api.reloadTimeMs = 2800;
        CHECK(logic.tickReload(api).type == gun::ReloadStateType::EmptyFinishing);
        CHECK(api.ammo.barrel); // LuaJ: even consumeAmmoFromPlayer returning 0 coerces to true
        CHECK(api.ammo.magazine == std::max(0, initialStock - 1)); CHECK(stock == 0); CHECK(calls == 2);
        api.reloadTimeMs = 2999;
        CHECK(logic.tickReload(api).type == gun::ReloadStateType::EmptyFinishing); CHECK(calls == 2);
        api.reloadTimeMs = 3000;
        CHECK(logic.tickReload(api).type == gun::ReloadStateType::NotReloading);
        CHECK(api.ammo.barrel); CHECK(calls == 2); CHECK(warnings.empty());
    }
}

TEST_CASE("Animation Java boolean parameters accept number nil and string using LuaJ truthiness") {
    auto clips = animation::parseAnimationFile(pack::parseJsonText(R"({"animations":{"probe":{"bones":{"root":{"position":{"0":[0,0,0],"4":[0,0,0]}}}}}})"));
    REQUIRE(clips.at("probe").duration == 4);
    for (auto literal : {"0", "nil", "'enabled'"}) {
        CAPTURE(literal);
        auto packs = registry("assets", std::string("local value = ") + literal + R"(
            return {initialize=function(self,c)
                local expected = c:getStateMachineParams().expected
                c:ensureTrackLineSize(1)
                local track = c:assignNewTrack(0)
                c:runAnimation('probe', track, value, PLAY_ONCE_HOLD, 0)
                c:holdAnimation(track)
                assert(c:findIdleTrack(0, value) == (expected and track or 1))
                c:runAnimation('probe', track, value, PLAY_ONCE_HOLD, 0)
                c:setAnimationProgress(track, .25, value)
                c:adjustAnimationProgress(track, .25, value)
                c:setShouldHideCrossHair(value)
                assert(c:shouldHideCrossHair() == expected)
            end}
        )");
        lua::AnimationContext context; bool expected = std::string_view(literal) != "nil";
        context.params["expected"] = expected;
        std::vector<std::string> warnings;
        lua::AnimationStateMachine machine(packs, clips, {"fixture", "attachment"}, context, [&](auto const& w) { warnings.push_back(w); });
        REQUIRE(machine.initialize(0));
        CHECK(machine.controller().info(0).progress == doctest::Approx(expected ? 2 : .5));
        CHECK(context.hideCrossHair == expected); CHECK(machine.healthy()); CHECK(warnings.empty());
    }
}
