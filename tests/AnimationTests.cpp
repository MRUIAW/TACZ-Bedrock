#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>

#include <glm/gtc/matrix_transform.hpp>

#include "model/Animation.h"
#include "model/GeoModel.h"
#include "model/GunModelRules.h"
#include "pack/GunAnimations.h"
#include "pack/GunPackParser.h"

using namespace tacz;

namespace {
animation::Channel channel(char const* text, char const* type = "scale") {
    auto json = nlohmann::json{{"animations", {{"test", {{"bones", {{"root", {{type, pack::parseJsonText(text)}}}}}}}}}};
    return animation::parseAnimationFile(json).at("test").channels.at(0);
}
bool near(glm::vec3 a, glm::vec3 b) { return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3{1e-5f})); }

class AnimationSource final : public pack::PackSource {
public:
    std::string label = "animation fixture";
    std::map<std::string, std::string> files;
    std::string const& name() const override { return label; }
    std::vector<std::string> list(std::string_view prefix) const override {
        std::vector<std::string> result;
        for (auto const& [path, text] : files) if (path.starts_with(prefix)) result.push_back(path);
        return result;
    }
    std::optional<std::string> read(std::string_view path) const override {
        auto it = files.find(std::string(path));
        return it == files.end() ? std::nullopt : std::optional{it->second};
    }
};
}

TEST_CASE("Animation: linear interpolation and clamped channel endpoints") {
    auto c = channel(R"({"0": [0, 10, 20], "2": [4, 20, 40]})");
    CHECK(near(c.evaluate(-1), {0, 10, 20}));
    CHECK(near(c.evaluate(0.5f), {1, 12.5f, 25}));
    CHECK(near(c.evaluate(2), {4, 20, 40}));
    CHECK(near(c.evaluate(3), {4, 20, 40}));
}

TEST_CASE("Animation: CustomInterpolator linear uses outgoing post and incoming pre") {
    auto c = channel(R"({"0": {"pre": [0,0,0], "post": [10,10,10]},
                          "1": {"pre": [20,20,20], "post": [30,30,30]}, "2": [40,40,40]})");
    CHECK(c.evaluate(-1).x == doctest::Approx(10));
    CHECK(c.evaluate(0).x == doctest::Approx(10));
    CHECK(c.evaluate(0.5f).x == doctest::Approx(15));
    CHECK(c.evaluate(0.999f).x == doctest::Approx(19.99));
    CHECK(c.evaluate(1).x == doctest::Approx(30));
    CHECK(c.evaluate(1.5f).x == doctest::Approx(35));
    auto last = channel(R"({"0": [0,0,0], "1": {"pre": [20,20,20], "post": [30,30,30]}})");
    CHECK(last.evaluate(1).x == doctest::Approx(30));
    CHECK(last.evaluate(2).x == doctest::Approx(30));
}

TEST_CASE("Animation: singleton pre/post and pre-only/post-only") {
    auto c = channel(R"({"0": {"pre": [1,2,3], "post": [4,5,6]}})");
    CHECK(near(c.evaluate(0), {1,2,3}));
    CHECK(near(c.evaluate(0.01f), {4,5,6}));
    CHECK(near(channel(R"({"0": {"pre": [1,2,3]}})").evaluate(10), {1,2,3}));
    CHECK(near(channel(R"({"0": {"post": [4,5,6]}})").evaluate(-1), {4,5,6}));
}

TEST_CASE("Animation: TACZ splineCurve hand-computed values and duplicated neighbours") {
    auto c = channel(R"({"0": [0,0,0], "1": {"post": [10,10,10], "lerp_mode": "catmullrom"},
                          "2": [20,20,20], "3": [40,40,40]})");
    // h1=h2=.5, h3=.125, h4=-.125, v0=10, v1=15 => 14.375.
    CHECK(c.evaluate(1.5f).x == doctest::Approx(14.375));
    // Either endpoint's mode selects spline; first neighbour is duplicated: v0=5, v1=10.
    CHECK(c.evaluate(0.5f).x == doctest::Approx(4.375));
    CHECK(c.evaluate(3).x == doctest::Approx(40));
    CHECK(c.evaluate(10).x == doctest::Approx(40));
    auto tail = channel(R"({"0": [0,0,0], "1": {"post": [10,10,10], "lerp_mode": "catmullrom"}})");
    CHECK(tail.evaluate(0.5f).x == doctest::Approx(5));
}

TEST_CASE("Animation: Catmull-Rom intentionally uses from pre and previous post") {
    auto c = channel(R"({"0": {"pre": [0,0,0], "post": [8,8,8]},
      "1": {"pre": [10,10,10], "post": [100,100,100], "lerp_mode": "catmullrom"},
      "2": {"pre": [20,20,20], "post": [200,200,200]}, "3": [40,40,40]})");
    // .5*10+.5*20+.125*(20-8)/2-.125*(40-10)/2 = 13.875, NOT outgoing 100.
    CHECK(c.evaluate(1.5f).x == doctest::Approx(13.875));
    auto last = channel(R"({"0": [0,0,0], "1": {"pre": [20,20,20], "post": [30,30,30], "lerp_mode": "catmullrom"}})");
    CHECK(last.evaluate(1).x == doctest::Approx(20));
    CHECK(last.evaluate(2).x == doctest::Approx(20));
}

TEST_CASE("Animation: units and coordinate conversion match Animations and listeners") {
    CHECK(near(channel("[16,32,-48]", "position").evaluate(0), {1,2,-3}));
    CHECK(near(channel("[90,-180,45]", "rotation").evaluate(0), {std::numbers::pi_v<float>/2, -std::numbers::pi_v<float>, std::numbers::pi_v<float>/4}));
    model::Bone bone;
    bone.pivot = {16,32,48};
    animation::BonePose pose;
    pose.translation = {1,2,-3};
    CHECK(near(glm::vec3(model::partTransform(bone, &pose) * glm::vec4{0,0,0,1}), {2,0,0}));
}

TEST_CASE("Animation: scalar scale and Molang handling") {
    CHECK(near(channel("1.5").evaluate(0), {1.5f,1.5f,1.5f}));
    int logs = 0;
    auto file = animation::parseAnimationFile(pack::parseJsonText(R"({"animations":{"a":{"bones":{
      "x":{"position":"query.anim_time", "rotation":["query.anim_time",90,0], "scale":1}
    }}}})"), [&](auto const&) { ++logs; });
    CHECK(logs == 2);
    CHECK(file.at("a").channels.size() == 2);
    animation::Pose pose;
    animation::evaluate(file.at("a"), 0, pose);
    CHECK_FALSE(pose.at("x").translation.has_value());
    CHECK(pose.at("x").rotation->x == 0);
    CHECK(pose.at("x").rotation->y == doctest::Approx(std::numbers::pi / 2));
}

TEST_CASE("Animation: loop hold stop use strict end boundary and zero length loops") {
    animation::Animation a;
    a.duration = 2;
    using enum animation::PlayType;
    CHECK(animation::playbackTime(a, 2, Loop) == 2.f);
    CHECK(animation::playbackTime(a, 2.5, Loop) == 0.5f);
    CHECK(animation::playbackTime(a, 7, Loop) == 1.f);
    CHECK(animation::playbackTime(a, 2, Hold) == 2.f);
    CHECK(animation::playbackTime(a, 3, Hold) == 2.f);
    CHECK(animation::playbackTime(a, 2, Stop) == 2.f);
    CHECK_FALSE(animation::playbackTime(a, 2.00001, Stop));
    a.duration = 0;
    CHECK(animation::playbackTime(a, 100, Loop) == 0.f);
    REQUIRE(animation::playbackTime(a, 100, Hold));
    CHECK(*animation::playbackTime(a, 100, Hold) > 0.f); // Runner.hold sentinel: end + 1 ns
    CHECK_FALSE(animation::playbackTime(a, 0.001, Stop));
}

TEST_CASE("Animation: holding a zero-length pre/post channel uses its post") {
    animation::Animation a;
    a.channels.push_back(channel(R"({"0":{"pre":[1,1,1],"post":[2,2,2]}})"));
    auto time = animation::playbackTime(a, 1, animation::PlayType::Hold);
    REQUIRE(time);
    CHECK(a.channels[0].evaluate(*time).x == doctest::Approx(2));
}

TEST_CASE("Animation: file loop metadata and duration follow TACZ maxEndTime not animation_length") {
    auto file = animation::parseAnimationFile(pack::parseJsonText(R"({"animations":{
      "loop":{"loop":true, "animation_length":99, "bones":{"root":{"scale":{"0":[1,1,1],"2":[2,2,2]}}}},
      "hold":{"loop":"hold_on_last_frame", "sound_effects":{"3":{"effect":"tacz:test"}}},
      "stop":{"loop":false}, "absent":{}
    }})"));
    CHECK(file.at("loop").playType == animation::PlayType::Loop);
    CHECK(file.at("loop").duration == 2);
    CHECK(file.at("hold").playType == animation::PlayType::Hold);
    CHECK(file.at("hold").duration == 3);
    CHECK(file.at("stop").playType == animation::PlayType::Stop);
    CHECK(file.at("absent").playType == animation::PlayType::Stop);
}

TEST_CASE("Animation: additive tracks keep missing channels and multiply scale") {
    auto file = animation::parseAnimationFile(pack::parseJsonText(R"({"animations":{
      "idle":{"bones":{"root":{"position":[16,32,48],"rotation":[1,2,3],"scale":[2,3,4]}}},
      "kick":{"bones":{"root":{"position":[16,0,0],"rotation":[2,0,0],"scale":[2,2,2]},"bolt":{"position":[0,0,16]}}}
    }})"));
    animation::Pose pose;
    animation::evaluate(file.at("idle"), 0, pose);
    animation::evaluate(file.at("kick"), 0, pose, true);
    CHECK(near(*pose.at("root").translation, {2,2,3}));
    CHECK(near(*pose.at("root").scale, {4,6,8}));
    CHECK(pose.at("root").rotation->x == doctest::Approx(3 * std::numbers::pi / 180));
    CHECK(near(*pose.at("bolt").translation, {0,0,1}));
}

TEST_CASE("Animation: rest pose regression and additional rotation order") {
    model::Bone bone;
    bone.pivot = {1,2,3};
    bone.rotation = {0.3f, 0.6f, 0.9f};
    animation::BonePose identity;
    identity.translation = {0,0,0};
    identity.rotation = {0,0,0};
    identity.scale = {1,1,1};
    auto rest = model::partTransform(bone);
    auto m = model::partTransform(bone, &identity);
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) CHECK(m[c][r] == doctest::Approx(rest[c][r]));
    identity.rotation = {0.4f, -0.2f, 0.1f};
    auto expected = glm::rotate(glm::rotate(glm::rotate(rest, .1f, {0,0,1}), -.2f, {0,1,0}), .4f, {1,0,0});
    m = model::partTransform(bone, &identity);
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) CHECK(m[c][r] == doctest::Approx(expected[c][r]));
}

TEST_CASE("Animation: additive Euler blending canonicalises the preceding quaternion") {
    auto file = animation::parseAnimationFile(pack::parseJsonText(R"({"animations":{
      "idle":{"bones":{"root":{"rotation":[0,0,370]}}},
      "kick":{"bones":{"root":{"rotation":[0,0,20]}}}
    }})"));
    animation::Pose pose;
    animation::evaluate(file.at("idle"), 0, pose);
    animation::evaluate(file.at("kick"), 0, pose, true);
    CHECK(pose.at("root").rotation->z == doctest::Approx(30 * std::numbers::pi / 180));
}

TEST_CASE("Animation: explicit defaults take precedence over legacy defaults and only fill missing names") {
    auto source = std::make_unique<AnimationSource>();
    source->files = {
        {"gunpack.meta.json", R"({"namespace":"test"})"},
        {"assets/test/animations/main.animation.json", R"({/* comment */ "animations":{"draw":{"bones":{"root":{"scale":2}}}}})"},
        {"assets/test/animations/fallback.animation.json", R"({"animations":{"draw":{"bones":{"root":{"scale":3}}},"inspect":{}}})"},
        {"assets/tacz/animations/rifle_default.animation.json", R"({"animations":{"idle":{}}})"}
    };
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(std::move(source));
    pack::GunPackRegistry registry;
    registry.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    auto display = pack::parseGunDisplay(pack::parseJsonText(R"({
      "animation":"test:main", "default_animation":"test:fallback", "use_default_animation":"rifle"
    })"));
    auto clips = pack::loadGunAnimations(registry, display);
    CHECK(clips.contains("inspect"));
    CHECK_FALSE(clips.contains("idle"));
    CHECK(clips.at("draw").channels[0].evaluate(0).x == 2);
    display.defaultAnimation.reset();
    clips = pack::loadGunAnimations(registry, display);
    CHECK(clips.contains("idle"));
    CHECK_FALSE(clips.contains("inspect"));
    display.animation.reset();
    CHECK(pack::loadGunAnimations(registry, display).empty()); // GunDisplayInstance's no-animation branch
}

#ifdef TACZ_DEFAULT_PACK_DIR
TEST_CASE("Animation: real AK47 pack parses, evaluates and bakes finite vertices") {
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(std::filesystem::u8path(TACZ_DEFAULT_PACK_DIR)));
    pack::GunPackRegistry registry;
    registry.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    auto display = registry.gunDisplay(*pack::ResourceId::parse("tacz:ak47_display"));
    REQUIRE(display);
    auto clips = pack::loadGunAnimations(registry, *display);
    for (auto name : {"static_idle", "draw", "shoot", "reload_tactical", "reload_empty", "inspect"}) CHECK(clips.contains(name));
    REQUIRE(clips.contains("shoot"));
    CHECK(clips.at("shoot").duration == doctest::Approx(0.8333)); // metadata says .7667!
    CHECK(clips.at("shoot").playType == animation::PlayType::Loop);
    auto text = registry.readFile(pack::assetPath(*display->model, "geo_models", ".json"));
    REQUIRE(text);
    auto geo = model::parseGeoModel(pack::parseJsonText(*text));
    auto visit = [](model::Bone const& b, model::Bone const* p) { return model::gunVisibility(b, p, {}); };
    auto rest = model::bake(geo, visit);
    auto empty = model::bake(geo, visit, {});
    REQUIRE(rest.size() == empty.size());
    for (size_t i = 0; i < rest.size(); ++i) {
        CHECK(near(rest[i].pos, empty[i].pos));
        CHECK(near(rest[i].normal, empty[i].normal));
    }
    for (auto const& [name, clip] : clips) {
        for (int i = 0; i <= 4; ++i) {
            animation::Pose pose;
            animation::evaluate(clip, clip.duration * i / 4, pose);
            auto vertices = model::bake(geo, visit, pose);
            REQUIRE(vertices.size() == rest.size());
            for (auto const& v : vertices) {
                if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y) || !std::isfinite(v.pos.z)
                    || !std::isfinite(v.normal.x) || !std::isfinite(v.normal.y) || !std::isfinite(v.normal.z)) {
                    FAIL_CHECK("Non-finite AK47 vertex in " << name);
                    break;
                }
            }
        }
    }
    // Repeatable game-independent CPU measurement; the renderer separately measures uploads.
    size_t vertexCount = 0;
    auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 600; ++frame) {
        animation::Pose pose;
        animation::evaluate(clips.at("static_idle"), 0, pose);
        animation::evaluate(clips.at("shoot"), (frame % 50) / 60.f, pose, true);
        vertexCount += model::bake(geo, visit, pose).size();
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(vertexCount == rest.size() * 600);
    std::cout << "[anim benchmark] AK47 evaluate+bake: " << ms / 600 << " ms/frame, " << rest.size() << " vertices (no GPU upload)\n";
}
#endif
