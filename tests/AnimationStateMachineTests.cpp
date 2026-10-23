#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <chrono>
#include <iostream>

#include "lua/AnimationStateMachine.h"
#include "pack/GunAnimations.h"
#include "pack/GunPackParser.h"

using namespace tacz;
namespace {
class ScriptSource final : public pack::PackSource {
public:
    std::string label = "animation script fixture";
    std::string source;
    std::string const& name() const override { return label; }
    std::vector<std::string> list(std::string_view prefix) const override {
        return prefix == "assets/" ? std::vector<std::string>{"assets/test/scripts/machine.lua"} : std::vector<std::string>{};
    }
    std::optional<std::string> read(std::string_view path) const override {
        if (path == "gunpack.meta.json") return R"({"namespace":"test"})";
        if (path == "assets/test/scripts/machine.lua") return source;
        return std::nullopt;
    }
};

pack::GunPackRegistry registryWith(std::string source) {
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    auto pack = std::make_unique<ScriptSource>();
    pack->source = std::move(source);
    sources.push_back(std::move(pack));
    pack::GunPackRegistry registry;
    registry.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    return registry;
}
animation::AnimationFile testClips() {
    return animation::parseAnimationFile(pack::parseJsonText(R"({"animations":{
      "static_idle":{"bones":{"root":{"scale":1}}},
      "draw":{"bones":{"root":{"scale":{"0":[2,2,2],"1":[2,2,2]}}}}
    }})"));
}
}

TEST_CASE("Animation VM: lifecycle, state entry/update/exit, nested triggers and root-table self") {
    auto packs = registryWith(R"(
        local a,b={},{}
        local M={counter=0}
        function M:initialize(c) c:ensureTrackLineSize(1); c:ensureTracksAmount(0,1); self.counter=1 end
        function M:states() assert(self.counter==1); return {a} end
        function a.entry(self,c) assert(self.counter==1); self.counter=2 end
        function a.update(self,c) c:trigger('advance') end
        function a.transition(self,c,input) if input=='advance' then assert(self.counter==2); return b end end
        function a.exit(self,c) self.counter=3 end
        function b.entry(self,c) assert(self.counter==3); c:runAnimation('draw',0,false,PLAY_ONCE_STOP,0) end
        function M:exit(c) self.counter=4 end
        function b.exit(self,c) assert(self.counter==4); c:setShouldHideCrossHair(true) end
        return M
    )");
    auto clips = testClips();
    lua::AnimationContext ctx;
    lua::AnimationStateMachine m(packs, clips, {"test","machine"}, ctx);
    REQUIRE(m.initialize(0));
    m.update(.1);
    CHECK(m.healthy());
    CHECK(m.controller().info(0).name == "draw");
    m.exit();
    CHECK(ctx.hideCrossHair);
}

TEST_CASE("Animation VM: tick movement classification follows TickAnimationEvent") {
    lua::AnimationContext ctx;
    ctx.sprinting = true;
    CHECK(lua::movementInput(ctx) == "idle"); // missing input always idles
    ctx.inputAvailable = true;
    CHECK(lua::movementInput(ctx) == "run");
    ctx.movingSlowly = true;
    CHECK(lua::movementInput(ctx) == "idle");
    ctx.sprinting = false;
    ctx.movingSlowly = false;
    ctx.moveMagnitude = .01f;
    CHECK(lua::movementInput(ctx) == "idle");
    ctx.moveMagnitude = .02f;
    CHECK(lua::movementInput(ctx) == "walk");
    ctx.inputUp = ctx.inputDown = true;
    ctx.moveMagnitude = 0;
    CHECK(lua::movementInput(ctx) == "idle"); // opposed keys are not movement
}

TEST_CASE("Animation VM: LuaJ compatibility, sandbox and neutral context API") {
    auto packs = registryWith(R"(
        return { initialize=function(self,c)
            assert(io==nil and os==nil and debug==nil and dofile==nil and loadfile==nil)
            assert(package.loadlib==nil and package.path=='' and package.cpath=='')
            assert(string.dump==nil and #package.searchers==1)
            assert(not pcall(require,'os'))
            assert(load(string.char(27)..'Lua')==nil)
            assert(unpack({7,8})==7 and math.pow(2,3)==8 and tostring(2/2)=='1')
            assert(bit32.bnot(0)==4294967295 and bit32.arshift(0x80000000,1)==0xc0000000)
            assert(PLAY_ONCE_HOLD==0 and PLAY_ONCE_STOP==1 and LOOP==2 and INPUT_BOLT=='blot')
            assert(AUTO==0 and SEMI==1 and BURST==2 and NOT_RELOADING==0 and TACTICAL_RELOAD_FINISHING==4)
            assert(not c:isAiming() and c:getAimingProgress()==0 and not c:isCrawl() and not c:shouldSlide())
            assert(not c:isOverHeat() and c:getHeatProgress()==0 and not c:isCharging())
            assert(c:getChargeProgress()==0 and c:getMaxCharge()==0 and c:getChargeThreshold()==0)
            assert(c:getAttachment('SCOPE')=='tacz:empty')
            assert(c:isInputJumping() and c:isCrouching() and c:isOnGround())
            c:anchorWalkDist(); assert(c:getWalkDist()==0)
            assert(c:getStateMachineParams().test==42)
            c:adjustClientShootInterval(-1.9)
            c:popShellFrom(2)
            c:setShouldHideCrossHair(true)
        end }
    )");
    auto clips = testClips();
    lua::AnimationContext ctx;
    ctx.inputJumping = ctx.crouching = ctx.onGround = true;
    ctx.params["test"] = 42;
    ctx.walkDist = 10;
    int64_t delta = 0;
    ctx.adjustShootInterval = [&](int64_t d) { delta = d; };
    std::vector<std::string> warnings;
    lua::AnimationStateMachine m(packs, clips, {"test","machine"}, ctx, [&](auto const& w) { warnings.push_back(w); });
    REQUIRE(m.initialize(0));
    CHECK(warnings.empty());
    CHECK(ctx.hideCrossHair);
    CHECK(ctx.walkAnchor == 10);
    CHECK(ctx.shells == std::vector<int>{2});
    CHECK(delta == -1);
}

TEST_CASE("Animation VM: read-only typed NBT accessor") {
    auto packs = registryWith(R"(
        return {initialize=function(self,c)
            local n=c:getNbtAccessor()
            assert(n:contains('GunCurrentAmmoCount',3) and n:contains('GunCurrentAmmoCount',99))
            assert(not n:contains('GunCurrentAmmoCount',8) and not n:contains('missing'))
            assert(n:getInt('GunCurrentAmmoCount')==29 and n:getDouble('GunCurrentAmmoCount')==29)
            assert(n:getString('GunId')=='tacz:ak47' and n:getString('missing')=='')
            assert(n:getCompound('missing')==nil and n:getCompound('nested'):getFloat('x')==1.5)
            assert(n.putInt==nil and n.putString==nil)
        end}
    )");
    auto clips = testClips();
    lua::AnimationContext ctx;
    ctx.readNbt = [](auto const& path, std::string const& key, int mode) -> nlohmann::json {
        if (path.empty() && key == "GunCurrentAmmoCount") return mode == 0 ? 3 : 29;
        if (path.empty() && key == "GunId") return mode == 0 ? nlohmann::json(8) : nlohmann::json("tacz:ak47");
        if (path.empty() && key == "nested" && mode == 0) return 10;
        if (path == std::vector<std::string>{"nested"} && key == "x") return mode == 0 ? 5.0 : 1.5;
        return {};
    };
    lua::AnimationStateMachine m(packs, clips, {"test","machine"}, ctx);
    CHECK(m.initialize(0));
}

TEST_CASE("Animation VM: callbacks are captured on state construction, not looked up each frame") {
    auto packs = registryWith(R"(
        local s={}
        function s.entry(self,c) s.update=function() error('must not replace captured update') end end
        function s.update(self,c) c:setShouldHideCrossHair(true) end
        return {states=function() return {s} end}
    )");
    auto clips = testClips();
    lua::AnimationContext ctx;
    lua::AnimationStateMachine m(packs, clips, {"test","machine"}, ctx);
    REQUIRE(m.initialize(0));
    m.update(.1);
    CHECK(m.healthy());
    CHECK(ctx.hideCrossHair);
}

TEST_CASE("Animation VM: syntax/runtime/runaway errors fall back without escaping") {
    for (std::string source : {"not valid lua", "return { initialize=function() error('broken pack') end }",
                              "return { initialize=function() while true do end end }",
                              "return { initialize=function(self,c) c:adjustClientShootInterval(9e18); c:adjustClientShootInterval(9e18) end }",
                              "return { states=function() return {{update=function() while true do end end}} end }"}) {
        auto packs = registryWith(source);
        auto clips = testClips();
        lua::AnimationContext ctx;
        std::vector<std::string> warnings;
        lua::AnimationStateMachine m(packs, clips, {"test","machine"}, ctx, [&](auto const& w) { warnings.push_back(w); });
        m.initialize(0);
        auto p = m.update(.1);
        CHECK_FALSE(m.healthy());
        REQUIRE(warnings.size() == 1);
        CHECK(warnings[0].find("test:machine") != std::string::npos);
        CHECK(m.controller().info(0).name == "static_idle");
        CHECK(p.at("root").scale->x == 1);
        CHECK_FALSE(m.trigger("shoot"));
    }
}

TEST_CASE("Animation VM: debug override completes and returns to the live state machine") {
    auto packs = registryWith(R"(return {
      initialize=function(self,c) c:ensureTrackLineSize(1); c:ensureTracksAmount(0,1); c:runAnimation('static_idle',0,false,LOOP,0) end
    })");
    auto clips = testClips();
    lua::AnimationContext ctx;
    lua::AnimationStateMachine m(packs, clips, {"test","machine"}, ctx);
    REQUIRE(m.initialize(0));
    CHECK_FALSE(m.playDebug("missing",0));
    REQUIRE(m.playDebug("draw",0));
    CHECK(m.update(.5).at("root").scale->x == 2);
    m.update(1.1);
    CHECK(m.update(1.2).at("root").scale->x == 1);
    CHECK(m.controller().info(0).name == "static_idle");
}

#ifdef TACZ_DEFAULT_PACK_DIR
TEST_CASE("Animation VM: real default and AK47 draw idle shoot reload inspect walk run sequence") {
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(std::filesystem::path(TACZ_DEFAULT_PACK_DIR)));
    pack::GunPackRegistry packs;
    packs.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    auto display = packs.gunDisplay({"tacz","ak47_display"});
    REQUIRE(display);
    auto clips = pack::loadGunAnimations(packs, *display);
    for (auto name : {"idle","walk_forward","walk_backward","walk_sideway","run_start","run","run_end"}) REQUIRE(clips.contains(name));
    for (auto script : {"default_state_machine", "ak47_state_machine"}) {
        CAPTURE(script);
        lua::AnimationContext ctx;
        ctx.ammo = ctx.maxAmmo = 30; ctx.barrel = ctx.ammoToConsume = ctx.onGround = true;
        ctx.shootInterval = 100;
        std::vector<std::string> warnings;
        lua::AnimationStateMachine m(packs, clips, {"tacz",script}, ctx, [&](auto const& w) { warnings.push_back(w); });
        REQUIRE(m.initialize(0));
        auto& controller = m.controller();
        auto main = controller.getTrack(0,9), movement = controller.getTrack(2,0);
        REQUIRE(m.trigger("draw"));
        CHECK(controller.info(main).name == "draw");
        for (int i = 0; i <= 120; ++i) m.update(i / 60.0);
        CHECK(controller.info(main).stopped);
        CHECK(controller.info(movement).name == "idle");
        m.setTime(2);
        REQUIRE(m.trigger("shoot"));
        auto kick = controller.getTrack(1,0);
        CHECK(controller.info(kick).name == "shoot");
        CHECK(ctx.shells == std::vector<int>{0});
        m.setTime(2.1);
        REQUIRE(m.trigger("shoot"));
        CHECK(controller.getTrack(1,1) != -1); // no restart: overlapping additive kick runners
        ctx.ammo = 0; ctx.barrel = false;
        m.setTime(2.2); REQUIRE(m.trigger("reload"));
        CHECK(controller.info(main).name == "reload_empty");
        ctx.reloadState = 1;
        for (int i = 133; i <= 420; ++i) m.update(i / 60.0);
        CHECK(controller.info(main).stopped);
        ctx.ammo = 30; ctx.barrel = true; ctx.reloadState = 0;
        REQUIRE(m.trigger("idle"));
        REQUIRE(m.trigger("inspect"));
        CHECK(controller.info(main).name == "inspect");
        CHECK(ctx.hideCrossHair);
        for (int i = 421; i <= 1200; ++i) m.update(i / 60.0);
        CHECK_FALSE(ctx.hideCrossHair);
        CHECK(controller.info(main).stopped);
        ctx.inputUp = true;
        REQUIRE(m.trigger("walk"));
        m.update(20.01);
        CHECK(controller.info(movement).name == "walk_forward");
        ctx.walkDist = .5;
        m.update(20.02);
        CHECK(controller.info(movement).progress > 0);
        ctx.sprinting = true;
        REQUIRE(m.trigger("run"));
        CHECK(controller.info(movement).name == "run_start");
        for (int i = 1202; i <= 1320; ++i) { ctx.walkDist += .03f; ctx.timestamp = i*1000/60; m.update(i/60.0); }
        CHECK(controller.info(movement).name == "run");
        REQUIRE(m.trigger("idle"));
        CHECK(controller.info(movement).name == "run_end");
        m.update(24); m.update(24.01);
        CHECK(controller.info(movement).name == "idle");
        if (std::string(script) == "ak47_state_machine") {
            ctx.fireMode = 1;
            m.update(24.02);
            CHECK(controller.info(controller.getTrack(0,17)).name == "switch_semi");
        }
        ctx.putAwayTime = .4f;
        REQUIRE(m.trigger("put_away"));
        CHECK(controller.info(main).name == "put_away");
        m.exit();
        CHECK(m.healthy());
        CHECK(warnings.empty());
    }
}

TEST_CASE("Animation VM: all real default-pack gun state machines initialize and accept action inputs") {
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(std::filesystem::path(TACZ_DEFAULT_PACK_DIR)));
    pack::GunPackRegistry packs;
    packs.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    size_t checked = 0;
    for (auto const& [id, gun] : packs.guns()) {
        if (!gun.index.display) continue;
        auto display = packs.gunDisplay(*gun.index.display);
        if (!display) continue;
        CAPTURE(id.toString());
        auto clips = pack::loadGunAnimations(packs, *display);
        lua::AnimationContext ctx;
        ctx.ammo = ctx.maxAmmo = gun.data.ammoAmount;
        ctx.barrel = ctx.onGround = ctx.ammoToConsume = true;
        ctx.shootInterval = 100;
        ctx.params = display->stateMachineParam;
        std::vector<std::string> warnings;
        lua::AnimationStateMachine m(packs, clips, display->stateMachine.value_or(pack::ResourceId{"tacz","default_state_machine"}),
            ctx, [&](auto const& w) { warnings.push_back(w); });
        REQUIRE(m.initialize(0));
        REQUIRE(m.trigger("draw"));
        m.update(.1); m.update(2); m.update(2.01);
        REQUIRE(m.trigger("shoot"));
        m.update(2.1);
        ctx.barrel = false; ctx.ammo = 0; ctx.reloadState = 1;
        REQUIRE(m.trigger("reload"));
        m.update(2.2); m.update(10);
        REQUIRE(m.trigger("cancel_reload"));
        ctx.reloadState = 0; ctx.ammo = ctx.maxAmmo; ctx.barrel = true;
        REQUIRE(m.trigger("inspect"));
        m.update(10.1); m.update(20); m.update(20.01);
        ctx.inputUp = true;
        REQUIRE(m.trigger("walk")); m.update(20.1);
        REQUIRE(m.trigger("run")); m.update(20.2); m.update(21); m.update(21.1);
        CHECK(m.healthy());
        for (auto const& w : warnings) MESSAGE(w);
        CHECK(warnings.empty());
        ++checked;
    }
    CHECK(checked == 54);
}

TEST_CASE("Animation VM: real AK47 frame update cost") {
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(std::filesystem::path(TACZ_DEFAULT_PACK_DIR)));
    pack::GunPackRegistry packs;
    packs.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    auto display = packs.gunDisplay({"tacz","ak47_display"});
    REQUIRE(display);
    auto clips = pack::loadGunAnimations(packs, *display);
    lua::AnimationContext ctx;
    ctx.ammo = ctx.maxAmmo = 30; ctx.barrel = ctx.onGround = true;
    lua::AnimationStateMachine m(packs, clips, {"tacz","ak47_state_machine"}, ctx);
    REQUIRE(m.initialize(0));
    REQUIRE(m.trigger("draw"));
    size_t bones = 0;
    auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 600; ++frame) {
        ctx.timestamp = frame*1000/60;
        ctx.walkDist += .02f;
        m.setTime(frame/60.0);
        if (frame % 6 == 0) m.trigger("shoot");
        if (frame % 3 == 0) m.trigger("walk");
        ctx.inputUp = true;
        bones += m.update(frame/60.0).size();
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    CHECK(m.healthy());
    CHECK(bones > 0);
    std::cout << "[anim benchmark] AK47 Lua+controller: " << ms/600 << " ms/frame (100 shots, no Minecraft/bake/GPU)\n";
}
#endif
