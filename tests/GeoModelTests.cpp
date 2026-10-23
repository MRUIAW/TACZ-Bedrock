#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include <glm/gtc/matrix_transform.hpp>

#include "model/GeoModel.h"
#include "model/GunModelRules.h"
#include "pack/GunPackLoader.h"
#include "pack/GunPackParser.h"

using namespace tacz;

namespace {

model::GeoModel parse(char const* text) { return model::parseGeoModel(pack::parseJsonText(text)); }

bool near(glm::vec3 a, glm::vec3 b, float eps = 1e-4f) { return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3(eps))); }

} // namespace

TEST_CASE("Geo: pivots follow TACZ convertPivot") {
    auto m = parse(R"({"format_version": "1.12.0", "minecraft:geometry": [{
        "description": {"texture_width": 16, "texture_height": 16},
        "bones": [
            {"name": "root", "pivot": [1, 20, 3]},
            {"name": "child", "parent": "root", "pivot": [4, 18, 1], "rotation": [90, 0, 0]}
        ]}]})");
    REQUIRE(m.bones.size() == 2);
    CHECK(m.roots == std::vector<int>{0});
    CHECK(near(m.bones[0].pivot, {1, 4, 3}));  // (x, 24 - y, z)
    CHECK(near(m.bones[1].pivot, {3, 2, -2})); // (x - px, py - y, z - pz)
    CHECK(m.bones[1].rotation.x == doctest::Approx(std::numbers::pi / 2));
    CHECK(m.path(1) == std::vector<int>{0, 1});
}

TEST_CASE("Geo: per-face cube maps JSON faces with the y flip and skips missing faces") {
    auto m = parse(R"({"format_version": "1.12.0", "minecraft:geometry": [{
        "description": {"texture_width": 32, "texture_height": 32},
        "bones": [{"name": "b", "pivot": [0, 0, 0], "cubes": [
            {"origin": [0, 0, 0], "size": [2, 4, 6], "uv": {
                "up":    {"uv": [0, 0], "uv_size": [2, 6]},
                "west":  {"uv": [8, 8], "uv_size": [6, 4]}
            }}
        ]}]}]})");
    auto const& quads = m.bones[0].quads;
    REQUIRE(quads.size() == 2);
    // JSON "west" feeds the +x (EAST) polygon, JSON "up" feeds the -y (DOWN) polygon.
    CHECK(near(quads[0].normal, {1, 0, 0}));
    CHECK(near(quads[1].normal, {0, -1, 0}));
    // EAST polygon UVs: (u2,v1) (u1,v1) (u1,v2) (u2,v2) over [8,14]x[8,12] / 32.
    CHECK(quads[0].uv[0].x == doctest::Approx(14.0 / 32));
    CHECK(quads[0].uv[0].y == doctest::Approx(8.0 / 32));
    CHECK(quads[0].uv[2].x == doctest::Approx(8.0 / 32));
    CHECK(quads[0].uv[2].y == doctest::Approx(12.0 / 32));
    // Cube spans y in [-4, 0] in the flipped frame (pivot.y - origin.y - size.y).
    for (auto const& p : quads[0].pos) {
        CHECK(p.x == doctest::Approx(2));
        CHECK(p.y <= 0);
        CHECK(p.y >= -4);
    }
}

TEST_CASE("Geo: rotated cube becomes a helper part at its pivot") {
    auto m = parse(R"({"format_version": "1.12.0", "minecraft:geometry": [{
        "description": {"texture_width": 16, "texture_height": 16},
        "bones": [{"name": "b", "pivot": [0, 24, 0], "cubes": [
            {"origin": [1, 20, 1], "size": [1, 1, 1], "pivot": [2, 22, 3], "rotation": [0, 45, 0], "uv": [0, 0]}
        ]}]}]})");
    REQUIRE(m.bones.size() == 2);
    auto const& helper = m.bones[1];
    CHECK(helper.name.empty());
    CHECK(helper.parent == 0);
    CHECK(near(helper.pivot, {2, 2, 3}));
    CHECK(helper.rotation.y == doctest::Approx(std::numbers::pi / 4));
    CHECK(helper.quads.size() == 6); // box UV: all six faces
    CHECK(m.bones[0].quads.empty());
}

TEST_CASE("Geo: errors are reported") {
    CHECK_THROWS_AS(parse(R"({"format_version": "1.10.0", "geometry.model": {}})"), model::ModelError);
    CHECK_THROWS_AS(parse(R"({"format_version": "1.12.0", "minecraft:geometry": [{"description": {"texture_width": 16, "texture_height": 16},
        "bones": [{"name": "a", "parent": "missing", "pivot": [0, 0, 0]}]}]})"), model::ModelError);
}

#ifdef TACZ_DEFAULT_PACK_DIR

TEST_CASE("AK47 model loads and idle_view sits at the camera") {
    std::vector<std::unique_ptr<pack::PackSource>> packs;
    packs.push_back(pack::openFolderPack(std::filesystem::u8path(TACZ_DEFAULT_PACK_DIR)));
    pack::GunPackRegistry registry;
    registry.load(std::move(packs), [](std::string const&) {}, [](std::string const&) {});

    auto const* display = registry.gunDisplay(*pack::ResourceId::parse("tacz:ak47_display"));
    REQUIRE(display);
    REQUIRE(display->model);
    REQUIRE(display->texture);
    CHECK(display->model->toString() == "tacz:gun/ak47_geo");
    CHECK(display->zoomModelFov == doctest::Approx(45));

    auto text = registry.readFile(pack::assetPath(*display->model, "geo_models", ".json"));
    REQUIRE(text);
    auto m = model::parseGeoModel(pack::parseJsonText(*text));
    CHECK(m.textureWidth == 128);
    CHECK(m.find("idle_view") >= 0);
    CHECK(m.find("righthand_pos") >= 0);
    REQUIRE(registry.readFile(pack::assetPath(*display->texture, "textures", ".png")));

    model::GunRenderState state;
    auto vertices = model::bake(m, [&](model::Bone const& b, model::Bone const* p) { return model::gunVisibility(b, p, state); });
    CHECK(vertices.size() > 1000);
    CHECK(vertices.size() % 4 == 0);

    // inverse(idle_view) * T(0, -1.5, 0) undoes the node's own transform chain.
    auto      path   = m.path(m.find("idle_view"));
    glm::mat4 toNode(1.0f);
    for (int i : path) toNode = toNode * model::partTransform(m.bones[i]);
    glm::mat4 undone = model::positioningInverse(m, path) * glm::translate(glm::mat4(1.0f), {0, -1.5f, 0}) * toNode;
    CHECK(near(glm::vec3(undone * glm::vec4(0, 0, 0, 1)), {0, 0, 0}, 1e-3f));
    CHECK(near(glm::vec3(undone * glm::vec4(1, 0, 0, 0)), {1, 0, 0}, 1e-3f));

    // After the full first-person transform the eye point is the view origin.
    glm::mat4 fp       = model::firstPersonIdleTransform(m);
    glm::vec3 eyeView  = glm::vec3(fp * toNode * glm::vec4(0, 0, 0, 1));
    CHECK(near(eyeView, {0, 0, 0}, 1e-3f));
}

#endif
