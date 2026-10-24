#include <doctest/doctest.h>

#include <limits>
#include <numbers>
#include <chrono>
#include <filesystem>
#include <iostream>

#include "pack/GunPackLoader.h"
#include "pack/GunPackParser.h"

#include "model/PlayerArms.h"
#include "model/GunModelRules.h"

using namespace tacz;

TEST_CASE("Arms: vanilla boxes retain neutral part pivots, slim width and sleeve inflation") {
    for (auto side : {model::Arm::Left, model::Arm::Right}) for (bool slim : {false, true}) {
        auto base = model::playerArmVertices(side, slim, false);
        auto sleeve = model::playerArmVertices(side, slim, true);
        REQUIRE(base.size() == 24);
        REQUIRE(sleeve.size() == 24);
        auto bounds = [](auto const& vertices) {
            glm::vec3 lo(100), hi(-100);
            for (auto const& v : vertices) { lo = glm::min(lo, v.pos); hi = glm::max(hi, v.pos); }
            return std::pair{lo, hi};
        };
        auto [lo, hi] = bounds(base);
        auto [slo, shi] = bounds(sleeve);
        CHECK((hi.x - lo.x) * 16 == doctest::Approx(slim ? 3 : 4));
        CHECK(lo.x * 16 == doctest::Approx(side == model::Arm::Left ? 4 : (slim ? -7 : -8)));
        CHECK(lo.y == doctest::Approx(0));
        CHECK(hi.y * 16 == doctest::Approx(12));
        CHECK(lo.z * 16 == doctest::Approx(-2));
        CHECK(hi.z * 16 == doctest::Approx(2));
        for (int axis = 0; axis < 3; ++axis) {
            CHECK((lo[axis] - slo[axis]) * 16 == doctest::Approx(0.25));
            CHECK((shi[axis] - hi[axis]) * 16 == doctest::Approx(0.25));
        }
        for (auto const& v : base) CHECK(glm::length(v.normal) == doctest::Approx(1));
        // Modern left island is NOT mirrored: outward normals agree with winding.
        for (size_t q = 0; q < base.size(); q += 4) {
            auto cross = glm::cross(base[q+1].pos - base[q].pos, base[q+2].pos - base[q].pos);
            CHECK(glm::dot(glm::normalize(cross), base[q].normal) == doctest::Approx(1));
        }
    }
}

TEST_CASE("Arms: each layer uses its vanilla UV island, including slim unwrap widths") {
    for (bool slim : {false, true}) for (auto side : {model::Arm::Left, model::Arm::Right})
        for (bool sleeve : {false, true}) {
            auto v = model::playerArmVertices(side, slim, sleeve);
            float u = side == model::Arm::Left ? (sleeve ? 48 : 32) : 40;
            float y = side == model::Arm::Left ? 48 : (sleeve ? 32 : 16);
            float width = slim ? 3 : 4;
            // NORTH face: [u+4, u+4+width] x [v+4, v+16].
            CHECK(v[16].uv.x * 64 == doctest::Approx(u + 4 + width));
            CHECK(v[16].uv.y * 64 == doctest::Approx(y + 4));
            CHECK(v[18].uv.x * 64 == doctest::Approx(u + 4));
            CHECK(v[18].uv.y * 64 == doctest::Approx(y + 16));
        }
}

TEST_CASE("Arms: same gun stack, animated ancestors and endpoint, THEN Z180") {
    model::GeoModel geo;
    model::Bone root; root.name = "root"; root.pivot = {2, 3, 4}; root.rotation = {0.2f, 0.3f, -0.4f};
    model::Bone hand; hand.name = "lefthand"; hand.parent = 0; hand.pivot = {-6, 19, 0};
    model::Bone pos; pos.name = "lefthand_pos"; pos.parent = 1; pos.pivot = {6, -11, 0};
    geo.bones = {root, hand, pos};
    animation::Pose pose;
    pose["root"].rotation = {0.1f, -0.2f, 0.3f};
    pose["lefthand"].translation = {0.2f, 0.3f, 0.4f};
    pose["lefthand"].rotation = {0.4f, 0.5f, 0.6f};
    pose["lefthand_pos"].scale = {1.2f, 0.8f, 1.1f};
    auto path = geo.path(2);
    auto got = model::playerArmTransform(geo, path, pose);
    REQUIRE(got);
    auto expected = model::partTransform(root, &pose["root"]) * model::partTransform(hand, &pose["lefthand"])
        * model::partTransform(pos, &pose["lefthand_pos"])
        * glm::rotate(glm::mat4(1), std::numbers::pi_v<float>, glm::vec3{0,0,1});
    for (int c=0;c<4;++c) for (int r=0;r<4;++r) CHECK((*got)[c][r] == doctest::Approx(expected[c][r]));
    CHECK(glm::determinant(glm::mat3(*got)) > 0); // no accidental left-hand reflection
    CHECK_FALSE(model::playerArmTransform(geo, {}, pose));
    std::vector<int> bad{0, 2};
    CHECK_FALSE(model::playerArmTransform(geo, bad, pose));
    geo.bones[1].name = "additional_magazine";
    CHECK_FALSE(model::playerArmTransform(geo, path, pose));
    geo.bones[1].name = "lefthand";
    pose["lefthand_pos"].scale = {0, 0, 0};
    CHECK_FALSE(model::playerArmTransform(geo, path, pose));
    pose["lefthand_pos"].scale = {1, 1, std::numeric_limits<float>::quiet_NaN()};
    CHECK_FALSE(model::playerArmTransform(geo, path, pose));
}

TEST_CASE("Arms: classic image admission and opaque base do not corrupt sleeve alpha") {
    CHECK(model::isClassicArmSkin(64, 64, 16384, false, false, "geometry.humanoid.custom"));
    CHECK(model::isClassicArmSkin(128, 128, 65536, false, false, "geometry.humanoid.customSlim"));
    CHECK_FALSE(model::isClassicArmSkin(64, 64, 16384, true, false, "geometry.humanoid.custom"));
    CHECK_FALSE(model::isClassicArmSkin(64, 64, 16384, false, true, "geometry.humanoid.custom"));
    CHECK_FALSE(model::isClassicArmSkin(64, 64, 16384, false, false, "geometry.custom.creature"));
    CHECK_FALSE(model::isClassicArmSkin(64, 32, 8192, false, false, "geometry.humanoid.custom"));
    CHECK_FALSE(model::isClassicArmSkin(64, 64, 0, false, false, "geometry.humanoid.custom"));
    for (unsigned width : {64u, 128u}) {
        unsigned scale = width / 64;
        std::vector<uint8_t> pixels(width * width * 4, 0);
        auto before = model::armSkinHash(pixels);
        model::makeArmSkinBaseOpaque(pixels, width, width);
        auto alpha = [&](unsigned x, unsigned y) { return pixels[(y * scale * width + x * scale)*4+3]; };
        CHECK(alpha(40,16) == 255);
        CHECK(alpha(32,48) == 255);
        CHECK(alpha(40,32) == 0);
        CHECK(alpha(48,48) == 0);
        CHECK(alpha(0,0) == 0);
        CHECK(model::armSkinHash(pixels) != before);
        auto stable = model::armSkinHash(pixels);
        model::makeArmSkinBaseOpaque(pixels, width, width);
        CHECK(model::armSkinHash(pixels) == stable);
        for (size_t i=0;i<pixels.size();i+=4) CHECK(pixels[i] == 0); // RGB never touched
    }
}

#ifdef TACZ_DEFAULT_PACK_DIR
TEST_CASE("Arms: AK47 slim arm placement matches TACZ's hand placeholder before and after animation") {
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(std::filesystem::path(TACZ_DEFAULT_PACK_DIR)));
    pack::GunPackRegistry registry;
    registry.load(std::move(sources), [](auto const&) {}, [](auto const&) {});
    auto const* display = registry.gunDisplay(*pack::ResourceId::parse("tacz:ak47_display"));
    REQUIRE(display);
    REQUIRE(display->model);
    auto text = registry.readFile(pack::assetPath(*display->model, "geo_models", ".json"));
    REQUIRE(text);
    auto geo = model::parseGeoModel(pack::parseJsonText(*text));
    animation::Pose pose;
    for (bool animated : {false, true}) {
        if (animated) {
            pose["root"].rotation = {0.1f, -0.2f, 0.3f};
            pose["lefthand"].translation = {0.3f, 0.1f, -0.4f};
            pose["righthand"].rotation = {0.5f, 0.2f, -0.1f};
        }
        for (auto side : {model::Arm::Left, model::Arm::Right}) {
            int index = geo.find(side == model::Arm::Left ? "lefthand_pos" : "righthand_pos");
            REQUIRE(index >= 0);
            auto path = geo.path(index);
            auto arm = model::playerArmTransform(geo, path, pose);
            REQUIRE(arm);
            glm::mat4 placeholder(1);
            for (auto part : path) {
                auto p = pose.find(geo.bones[part].name);
                placeholder *= model::partTransform(geo.bones[part], p == pose.end() ? nullptr : &p->second);
            }
            glm::vec3 armLo(100), armHi(-100), refLo(100), refHi(-100);
            for (auto const& v : model::playerArmVertices(side, true, false)) {
                auto p = glm::vec3(*arm * glm::vec4(v.pos, 1));
                armLo = glm::min(armLo, p); armHi = glm::max(armHi, p);
            }
            for (auto const& q : geo.bones[index].quads) for (auto const& v : q.pos) {
                auto p = glm::vec3(placeholder * glm::vec4(v / 16.0f, 1));
                refLo = glm::min(refLo, p); refHi = glm::max(refHi, p);
            }
            for (int axis=0;axis<3;++axis) {
                CHECK(armLo[axis] == doctest::Approx(refLo[axis]).epsilon(1e-4));
                CHECK(armHi[axis] == doctest::Approx(refHi[axis]).epsilon(1e-4));
            }
        }
    }
    auto left = geo.path(geo.find("lefthand_pos")), right = geo.path(geo.find("righthand_pos"));
    auto start = std::chrono::steady_clock::now();
    float consumed = 0;
    for (int i=0;i<10000;++i) {
        pose["lefthand"].rotation = {0.0001f * i, 0.2f, -0.1f};
        consumed += (*model::playerArmTransform(geo, left, pose))[0][0];
        consumed += (*model::playerArmTransform(geo, right, pose))[0][0];
    }
    CHECK(std::isfinite(consumed));
    std::cout << "[arm benchmark] AK47 both cached paths: "
        << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count()/10000
        << " ms/frame (pure matrices, no Minecraft/skin/upload/draw)\n";
}
#endif
