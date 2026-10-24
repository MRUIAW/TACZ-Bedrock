#include "model/GeoModel.h"

#include <cmath>
#include <numbers>
#include <optional>

#include <glm/gtc/matrix_transform.hpp>

namespace tacz::model {
namespace {

using json = nlohmann::json;

constexpr float DegToRad = std::numbers::pi_v<float> / 180.0f;

[[noreturn]] void fail(std::string message) { throw ModelError(std::move(message)); }

std::optional<glm::vec3> optVec3(json const& obj, std::string_view key, std::string_view owner) {
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return std::nullopt;
    if (!it->is_array() || it->size() < 3) fail(std::string(owner) + ": '" + std::string(key) + "' is not a 3-number array");
    glm::vec3 v;
    for (int i = 0; i < 3; ++i) {
        if (!(*it)[i].is_number()) fail(std::string(owner) + ": '" + std::string(key) + "' is not a 3-number array");
        v[i] = (*it)[i].get<float>();
    }
    return v;
}

glm::vec3 vec3(json const& obj, std::string_view key, std::string_view owner) {
    auto v = optVec3(obj, key, owner);
    if (!v) fail(std::string(owner) + ": missing '" + std::string(key) + "'");
    return *v;
}

glm::vec2 vec2(json const& obj, std::string_view key, std::string_view owner) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || it->size() < 2 || !(*it)[0].is_number() || !(*it)[1].is_number()) {
        fail(std::string(owner) + ": '" + std::string(key) + "' is not a 2-number array");
    }
    return {(*it)[0].get<float>(), (*it)[1].get<float>()};
}

float number(json const& obj, std::string_view key, float fallback) {
    auto it = obj.find(key);
    return it != obj.end() && it->is_number() ? it->get<float>() : fallback;
}

// A face of BedrockCubeBox / BedrockCubePerFace: corner indices into the 8 cube corners,
// the outward normal, and (per-face UV) which JSON face supplies the UV. The y flip of the
// Java convention swaps up/down and east/west (FaceUVsItem.getFace).
struct FaceSpec {
    std::array<int, 4> corners;
    glm::vec3          normal;
    char const*        jsonFace;
};

// Same order as TACZ's polygons array: EAST, WEST, DOWN, UP, NORTH, SOUTH.
constexpr std::array<FaceSpec, 6> Faces{{
    {{5, 1, 2, 6}, {1, 0, 0}, "west"},
    {{0, 4, 7, 3}, {-1, 0, 0}, "east"},
    {{5, 4, 0, 1}, {0, -1, 0}, "up"},
    {{2, 3, 7, 6}, {0, 1, 0}, "down"},
    {{1, 0, 3, 2}, {0, 0, -1}, "north"},
    {{4, 5, 6, 7}, {0, 0, 1}, "south"},
}};

// Cube corners after inflate; mirror (box UV only) swaps the x extents first.
std::array<glm::vec3, 8> corners(glm::vec3 origin, glm::vec3 size, float inflate, bool swapX) {
    glm::vec3 lo = origin - glm::vec3(inflate);
    glm::vec3 hi = origin + size + glm::vec3(inflate);
    if (swapX) std::swap(lo.x, hi.x);
    return {{
        {lo.x, lo.y, lo.z},
        {hi.x, lo.y, lo.z},
        {hi.x, hi.y, lo.z},
        {lo.x, hi.y, lo.z},
        {lo.x, lo.y, hi.z},
        {hi.x, lo.y, hi.z},
        {hi.x, hi.y, hi.z},
        {lo.x, hi.y, hi.z},
    }};
}

// BedrockPolygon: UVs (u2,v1) (u1,v1) (u1,v2) (u2,v2); mirror reverses the vertices and flips normal x.
Quad makeQuad(std::array<glm::vec3, 8> const& c, FaceSpec const& face, glm::vec4 uvRect, glm::vec2 texSize, bool mirror) {
    Quad q;
    for (int k = 0; k < 4; ++k) q.pos[k] = c[face.corners[k]];
    float u1 = uvRect.x / texSize.x, v1 = uvRect.y / texSize.y, u2 = uvRect.z / texSize.x, v2 = uvRect.w / texSize.y;
    q.uv     = {{{u2, v1}, {u1, v1}, {u1, v2}, {u2, v2}}};
    q.normal = face.normal;
    if (mirror) {
        std::swap(q.pos[0], q.pos[3]);
        std::swap(q.pos[1], q.pos[2]);
        std::swap(q.uv[0], q.uv[3]);
        std::swap(q.uv[1], q.uv[2]);
        q.normal.x = -q.normal.x;
    }
    return q;
}

// BedrockCubeBox: the standard Minecraft box unwrap; sizes are truncated to int for the layout.
void boxCube(std::vector<Quad>& out, glm::vec3 origin, glm::vec3 size, float inflate, bool mirror, glm::vec2 uv, glm::vec2 texSize) {
    auto  c  = corners(origin, size, inflate, mirror);
    float dx = std::trunc(size.x), dy = std::trunc(size.y), dz = std::trunc(size.z);
    float p1 = uv.x + dz, p2 = uv.x + dz + dx, p3 = uv.x + dz + dx + dx, p4 = uv.x + dz + dx + dz,
          p5 = uv.x + dz + dx + dz + dx, p6 = uv.y + dz, p7 = uv.y + dz + dy, p8 = uv.y, p9 = uv.x;
    std::array<glm::vec4, 6> rects{{
        {p2, p6, p4, p7}, // east
        {p9, p6, p1, p7}, // west
        {p1, p8, p2, p6}, // down
        {p2, p6, p3, p8}, // up
        {p1, p6, p2, p7}, // north
        {p4, p6, p5, p7}, // south
    }};
    for (size_t i = 0; i < Faces.size(); ++i) out.push_back(makeQuad(c, Faces[i], rects[i], texSize, mirror));
}

// BedrockCubePerFace: each face reads its own uv / uv_size; a missing face is not drawn.
// Per-face cubes ignore mirror.
void perFaceCube(std::vector<Quad>& out, glm::vec3 origin, glm::vec3 size, float inflate, json const& uvObj, glm::vec2 texSize) {
    auto c = corners(origin, size, inflate, false);
    for (auto const& face : Faces) {
        auto it = uvObj.find(face.jsonFace);
        if (it == uvObj.end() || it->is_null()) continue;
        if (!it->is_object()) fail(std::string("cube face '") + face.jsonFace + "' is not an object");
        glm::vec2 uv     = vec2(*it, "uv", face.jsonFace);
        glm::vec2 uvSize = vec2(*it, "uv_size", face.jsonFace);
        out.push_back(makeQuad(c, face, {uv.x, uv.y, uv.x + uvSize.x, uv.y + uvSize.y}, texSize, false));
    }
}

// TACZ's format check (BedrockVersion.isNewVersion): three parts, minor >= 12.
bool isNewFormat(std::string const& version) {
    auto first  = version.find('.');
    auto second = first == std::string::npos ? std::string::npos : version.find('.', first + 1);
    if (second == std::string::npos) return false;
    try {
        return std::stoi(version.substr(first + 1, second - first - 1)) >= 12;
    } catch (...) {
        return false;
    }
}

} // namespace

std::vector<int> GeoModel::path(int bone) const {
    std::vector<int> out;
    for (int i = bone; i >= 0; i = bones[i].parent) out.insert(out.begin(), i);
    return out;
}

GeoModel parseGeoModel(json const& j) {
    if (!j.is_object()) fail("geometry file is not a JSON object");
    std::string version = j.value("format_version", std::string{});
    if (version == "1.10.0") fail("legacy 1.10.0 geometry is not supported yet");
    if (!isNewFormat(version)) fail("unsupported format_version '" + version + "'");

    auto geometries = j.find("minecraft:geometry");
    if (geometries == j.end() || !geometries->is_array() || geometries->empty()) fail("missing minecraft:geometry");
    auto const& geo = (*geometries)[0];

    GeoModel model;
    if (auto desc = geo.find("description"); desc != geo.end() && desc->is_object()) {
        model.identifier    = desc->value("identifier", std::string{});
        model.textureWidth  = desc->value("texture_width", 0);
        model.textureHeight = desc->value("texture_height", 0);
    }
    if (model.textureWidth <= 0 || model.textureHeight <= 0) fail("texture_width / texture_height must be positive");
    glm::vec2 texSize{static_cast<float>(model.textureWidth), static_cast<float>(model.textureHeight)};

    auto bonesIt = geo.find("bones");
    if (bonesIt == geo.end() || bonesIt->is_null()) return model;
    if (!bonesIt->is_array()) fail("'bones' is not an array");
    auto const& bones = *bonesIt;

    // First pass: one part per bone name (the first definition wins the slot, as with putIfAbsent),
    // remembering the raw pivot used for the coordinate conversion.
    std::vector<glm::vec3> rawPivot;
    for (auto const& b : bones) {
        if (!b.is_object() || !b.contains("name") || !b["name"].is_string()) fail("bone without a name");
        auto name = b["name"].get<std::string>();
        if (model.byName.contains(name)) continue;
        int index = static_cast<int>(model.bones.size());
        model.byName.emplace(name, index);
        Bone bone;
        bone.name        = name;
        bone.illuminated = name.ends_with("_illuminated");
        model.bones.push_back(std::move(bone));
        rawPivot.push_back(vec3(b, "pivot", name));
    }

    // Second pass: pivots, rotations, hierarchy and cubes.
    for (auto const& b : bones) {
        auto        name  = b["name"].get<std::string>();
        int const   index = model.byName.at(name);
        glm::vec3   pivot = vec3(b, "pivot", name);
        bool const  boneMirror = b.value("mirror", false);
        std::string parentName;
        if (auto p = b.find("parent"); p != b.end() && p->is_string()) parentName = p->get<std::string>();

        // convertPivot: root (x, 24 - y, z); child (x - px, py - y, z - pz).
        if (parentName.empty()) {
            model.bones[index].pivot  = {pivot.x, 24.0f - pivot.y, pivot.z};
            model.bones[index].parent = -1;
            model.roots.push_back(index);
        } else {
            int parent = model.find(parentName);
            if (parent < 0) fail("bone '" + name + "' has unknown parent '" + parentName + "'");
            glm::vec3 pp              = rawPivot[parent];
            model.bones[index].pivot  = {pivot.x - pp.x, pp.y - pivot.y, pivot.z - pp.z};
            model.bones[index].parent = parent;
            model.bones[parent].children.push_back(index);
        }
        if (auto rot = optVec3(b, "rotation", name)) model.bones[index].rotation = *rot * DegToRad;

        auto cubes = b.find("cubes");
        if (cubes == b.end() || cubes->is_null()) continue;
        if (!cubes->is_array()) fail("bone '" + name + "': 'cubes' is not an array");
        for (auto const& cube : *cubes) {
            if (!cube.is_object()) fail("bone '" + name + "': cube is not an object");
            glm::vec3 origin   = vec3(cube, "origin", name);
            glm::vec3 size     = vec3(cube, "size", name);
            float     inflate  = number(cube, "inflate", 0);
            bool      mirror   = cube.contains("mirror") && cube["mirror"].is_boolean() ? cube["mirror"].get<bool>() : boneMirror;
            auto      rotation = optVec3(cube, "rotation", name);
            auto      uvIt     = cube.find("uv");
            if (uvIt == cube.end() || !(uvIt->is_array() || uvIt->is_object())) fail("bone '" + name + "': cube has no uv");

            // The cube's local frame: the bone pivot, or for a rotated cube its own pivot
            // inside a helper part (convertOrigin / convertPivot(bone, cube)).
            std::vector<Quad>* target = &model.bones[index].quads;
            glm::vec3          frame  = pivot;
            if (rotation) {
                auto cubePivot = optVec3(cube, "pivot", name);
                if (!cubePivot) fail("bone '" + name + "': rotated cube has no pivot");
                Bone helper;
                helper.parent   = index;
                helper.pivot    = {cubePivot->x - pivot.x, pivot.y - cubePivot->y, cubePivot->z - pivot.z};
                helper.rotation = *rotation * DegToRad;
                int helperIndex = static_cast<int>(model.bones.size());
                model.bones.push_back(std::move(helper));
                model.bones[index].children.push_back(helperIndex);
                target = &model.bones[helperIndex].quads;
                frame  = *cubePivot;
            }
            glm::vec3 local{origin.x - frame.x, frame.y - origin.y - size.y, origin.z - frame.z};
            if (uvIt->is_array()) {
                boxCube(*target, local, size, inflate, mirror, vec2(cube, "uv", name), texSize);
            } else {
                perFaceCube(*target, local, size, inflate, *uvIt, texSize);
            }
        }
    }
    return model;
}

glm::mat4 partTransform(Bone const& bone, animation::BonePose const* pose) {
    glm::vec3 offset = pose && pose->translation ? *pose->translation * glm::vec3{1, -1, 1} : glm::vec3{0};
    glm::mat4 m = glm::translate(glm::mat4(1.0f), offset + bone.pivot / 16.0f);
    if (bone.rotation.z != 0) m = glm::rotate(m, bone.rotation.z, {0, 0, 1});
    if (bone.rotation.y != 0) m = glm::rotate(m, bone.rotation.y, {0, 1, 0});
    if (bone.rotation.x != 0) m = glm::rotate(m, bone.rotation.x, {1, 0, 0});
    // BedrockPart: rest rotation, then additionalQuaternion (MathUtil.toQuaternion),
    // then scale. In particular this is NOT Rz(rest.z + animation.z) * ... .
    if (pose && pose->rotation) {
        auto r = *pose->rotation;
        m = glm::rotate(m, r.z, {0, 0, 1});
        m = glm::rotate(m, r.y, {0, 1, 0});
        m = glm::rotate(m, r.x, {1, 0, 0});
    }
    if (pose && pose->scale) m = glm::scale(m, *pose->scale);
    return m;
}

glm::mat4 positioningInverse(GeoModel const& model, std::vector<int> const& path) {
    glm::mat4 m(1.0f);
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        Bone const& part = model.bones[*it];
        m                = glm::rotate(m, -part.rotation.x, {1, 0, 0});
        m                = glm::rotate(m, -part.rotation.y, {0, 1, 0});
        m                = glm::rotate(m, -part.rotation.z, {0, 0, 1});
        if (part.parent >= 0) {
            m = glm::translate(m, -part.pivot / 16.0f);
        } else {
            m = glm::translate(m, {-part.pivot.x / 16.0f, 1.5f - part.pivot.y / 16.0f, -part.pivot.z / 16.0f});
        }
    }
    return m;
}

namespace {

void bakeBone(GeoModel const& model, int index, glm::mat4 const& parentMatrix, VisitFn const& visit, animation::Pose const& pose, std::vector<Vertex>& out) {
    Bone const& bone   = model.bones[index];
    Bone const* parent = bone.parent >= 0 ? &model.bones[bone.parent] : nullptr;
    if (visit(bone, parent) == Visit::Hide) return;
    auto it = pose.find(bone.name);
    glm::mat4 m = parentMatrix * partTransform(bone, it == pose.end() ? nullptr : &it->second);
    glm::mat3 n(m);
    // Normal matrix for non-uniform animated scales; zero scales collapse the geometry.
    if (std::abs(glm::determinant(n)) > 1e-12f) n = glm::transpose(glm::inverse(n));
    for (auto const& quad : bone.quads) {
        glm::vec3 normal = n * quad.normal;
        normal = glm::dot(normal, normal) > 1e-12f ? glm::normalize(normal) : quad.normal;
        for (int k = 0; k < 4; ++k) {
            out.push_back({glm::vec3(m * glm::vec4(quad.pos[k] / 16.0f, 1.0f)), quad.uv[k], normal});
        }
    }
    for (int child : bone.children) bakeBone(model, child, m, visit, pose, out);
}

} // namespace

std::vector<Vertex> bake(GeoModel const& model, VisitFn const& visit, animation::Pose const& pose) {
    std::vector<Vertex> out;
    for (int root : model.roots) bakeBone(model, root, glm::mat4(1.0f), visit, pose, out);
    return out;
}

std::vector<Vertex> boxVertices(glm::vec3 origin, glm::vec3 size, glm::vec2 uv,
    glm::vec2 textureSize, float inflate, bool mirror) {
    std::vector<Quad> quads;
    boxCube(quads, origin, size, inflate, mirror, uv, textureSize);
    std::vector<Vertex> out;
    out.reserve(24);
    for (auto const& quad : quads) {
        for (int i = 0; i < 4; ++i) out.push_back({quad.pos[i] / 16.0f, quad.uv[i], quad.normal});
    }
    return out;
}

} // namespace tacz::model
