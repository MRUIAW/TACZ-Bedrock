#include <doctest/doctest.h>
#include <cmath>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include "model/FunctionalBones.h"
#include "model/Aiming.h"
#include "model/CameraRecoil.h"
#include "model/AnimationController.h"
#include "model/GunModelRules.h"
#include "pack/GunPackParser.h"
#include "pack/GunAnimations.h"
#include "lua/AnimationStateMachine.h"

using namespace tacz;
namespace {
void vectorNear(glm::vec3 a, glm::vec3 b, double eps = 1e-5) {
    for (int i=0;i<3;++i) CHECK(a[i] == doctest::Approx(b[i]).epsilon(eps));
}
void matrixNear(glm::mat4 const& a, glm::mat4 const& b) {
    for (int c=0;c<4;++c) for (int r=0;r<4;++r) CHECK(a[c][r] == doctest::Approx(b[c][r]).epsilon(1e-5));
}
#ifdef TACZ_DEFAULT_PACK_DIR
nlohmann::json geometry() {
    std::ifstream input{std::filesystem::path(TACZ_DEFAULT_PACK_DIR)/"assets/tacz/geo_models/gun/ak47_geo.json"};
    return nlohmann::json::parse(input);
}
pack::GunPackRegistry registry() {
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(pack::openFolderPack(TACZ_DEFAULT_PACK_DIR));
    pack::GunPackRegistry packs;
    packs.load(std::move(sources),[](auto const&){},[](auto const&){});
    return packs;
}
#endif
}

TEST_CASE("Native recoil mapping: TACZ positive curve pitch is UP; positive yaw curve is LEFT") {
    // Java look delta is -deltaCurve for both axes; native input negates ONLY pitch.
    // 0.3.14 runtime trace frame1->2: pitch -6.547424 -> -6.016251 for input -.531189;
    // yaw 48.878433 ->49.038773 for input +.160341 (deferred actor copy).
    auto turn = model::bedrockRecoilTurn({-1,0});
    CHECK(turn.pitch == 1); CHECK(turn.yaw == 0);
    turn = model::bedrockRecoilTurn({0,-1});
    CHECK(turn.pitch == 0); CHECK(turn.yaw == -1);
    // Both games yaw 0 faces +Z; yaw +90 points -X (right while facing +Z).
    auto javaRayX = -std::sin(glm::radians(-1.f));
    CHECK(javaRayX > 0); // negative yaw -> +X -> LEFT
    CHECK(model::bedrockRecoilTurn({0,.23}).yaw == .23); // AK47 negative key -> right
    CHECK(model::bedrockRecoilTurn({0,-.18}).yaw == -.18); // positive key -> left
}

TEST_CASE("ADS MDV options: coefficient, base multiplier and clamping") {
    for (float c : {.5f,1.f,1.33f,2.f}) {
        float fov = model::aimingFov(70,1.33f,.7f);
        auto expected = std::atan(c*std::tan(glm::radians(fov)*.5f))/std::atan(c*std::tan(glm::radians(70.f)*.5f));
        CHECK(model::aimingSensitivity(70,1.33f,.7f,c,1.5f) == doctest::Approx(expected*1.35));
    }
    CHECK(model::aimingSensitivity(70,1.33f,0,1.33f,2) == 1);
    CHECK(model::aimingSensitivity(70,1.33f,1,1.33f,0) == 0);
    CHECK(model::aimingSensitivity(70,1.33f,1,1.33f,5) == model::aimingSensitivity(70,1.33f,1,1.33f,2));
}

TEST_CASE("Camera listener: x/y/-z, log-space blending, axis-angle ADS scale without recoil sqrt cap") {
    auto r = glm::radians(glm::vec3(10,20,30));
    auto q = model::cameraBoneRotation(r);
    vectorNear(model::quaternionAngles(q),glm::vec3(r.x,r.y,-r.z));
    auto half = model::scaledCameraRotation(model::cameraBoneRotation({0,0,.2f}),1,4);
    vectorNear(model::quaternionAngles(half),{0,0,-.1f});
    auto mixed = model::blendCameraRotation(glm::angleAxis(.2f,glm::vec3(1,0,0)),glm::angleAxis(.2f,glm::vec3(0,1,0)));
    auto expected = glm::angleAxis(std::sqrt(.08f),glm::normalize(glm::vec3(1,1,0)));
    CHECK(std::abs(glm::dot(mixed,expected)) == doctest::Approx(1));
    animation::Pose a,b;
    a["camera"].rotation = glm::vec3(.2,0,0);
    b["camera"].rotation = glm::vec3(0,.2,0);
    animation::compose(b,a,true);
    CHECK(std::abs(glm::dot(model::cameraBoneRotation(*a["camera"].rotation),expected)) == doctest::Approx(1));
}

TEST_CASE("Camera event: add pitch/yaw/roll to rendered orientation, never change camera eye") {
    auto view = glm::rotate(glm::mat4(1),.25f,{0,0,1});
    view = glm::rotate(view,.4f,{1,0,0}); view = glm::rotate(view,2.f,{0,1,0});
    view = glm::translate(view,{-40,-72,19});
    auto q = model::cameraBoneRotation({.1f,.2f,.3f});
    auto expected = glm::rotate(glm::mat4(1),-.05f,{0,0,1});
    expected = glm::rotate(expected,.5f,{1,0,0}); expected = glm::rotate(expected,2.2f,{0,1,0});
    expected = glm::translate(expected,{-40,-72,19});
    auto result = model::applyCameraAngles(view,q);
    matrixNear(result,expected);
    vectorNear(glm::inverse(result)[3],glm::inverse(view)[3]);
}

TEST_CASE("Constraint listeners: component max, translation x16, positive-degree coefficients, no geometric offset") {
    animation::Pose pose, source;
    pose["constraint"].translation = glm::vec3(.1f)/16.f;
    pose["constraint"].rotation = glm::radians(glm::vec3(.2f,.2f,.4f));
    source["constraint"].translation = glm::vec3(.15f,.05f,.4f)/16.f;
    source["constraint"].rotation = glm::radians(glm::vec3(.3f,.3f,.3f));
    animation::compose(source,pose,true);
    vectorNear(*pose["constraint"].translation*16.f,{.15f,.1f,.4f});
    vectorNear(glm::degrees(*pose["constraint"].rotation),{.3f,.3f,.4f});
    CHECK(model::positiveDegrees(glm::radians(-10.f)) == doctest::Approx(350));
    model::Bone bone; bone.name="constraint"; bone.pivot={1,2,3};
    matrixNear(model::partTransform(bone,&pose["constraint"]),model::partTransform(bone));
}

TEST_CASE("Constraint path: excludes own coefficients/scales, includes ancestors animation BEFORE rest rotation") {
    auto model = model::parseGeoModel(pack::parseJsonText(R"({"format_version":"1.12.0","minecraft:geometry":[{
      "description":{"texture_width":16,"texture_height":16},"bones":[
      {"name":"root","pivot":[0,0,0],"rotation":[0,0,30]},
      {"name":"constraint","parent":"root","pivot":[16,0,0]}]}]})"));
    animation::Pose pose;
    pose["root"].translation=glm::vec3(1,2,3);
    pose["root"].rotation=glm::vec3(0,0,.2f);
    pose["root"].scale=glm::vec3(9);
    pose["constraint"].translation=glm::vec3(.1f)/16.f;
    pose["constraint"].rotation=glm::radians(glm::vec3(.2f));
    auto c = model::constraintTransform(model,pose);
    REQUIRE(c.present);
    vectorNear(c.rest,{std::cos(glm::radians(30.f)),std::sin(glm::radians(30.f)),0});
    vectorNear(c.animated,glm::vec3(1,-2,3)+glm::vec3(std::cos(glm::radians(30.f)+.2f),std::sin(glm::radians(30.f)+.2f),0));
    vectorNear(c.rotation,{0,0,.2f});
    vectorNear(c.translationCoefficient,{.1f,.1f,.1f});
    vectorNear(c.rotationCoefficient,{.2f,.2f,.2f});
}

TEST_CASE("Constraint counter-transform: basis direction signs, aim weight, pivot and Rx Ry Rz order") {
    model::ConstraintTransform c;
    c.present=true; c.rest={1,2,3}; c.animated={2,4,6};
    c.translationCoefficient={.1,.2,.3};
    auto m=glm::rotate(glm::mat4(1),.7f,{0,0,1});
    auto delta=glm::mat3(m)*(c.rest-c.animated)*glm::vec3(-.9f,-.8f,.7f);
    auto expected=m; expected[3]+=glm::vec4(-delta.x,-delta.y,delta.z,0)*.5f;
    matrixNear(model::applyConstraint(m,c,.5),expected);
    matrixNear(model::applyConstraint(m,c,0),m);
    c.translationCoefficient=glm::vec3(1); c.rotation={.1,.2,.3};
    expected=glm::translate(m,c.animated+glm::vec3(0,1.5,0));
    expected=glm::rotate(expected,-.05f,{1,0,0}); expected=glm::rotate(expected,-.1f,{0,1,0});
    expected=glm::rotate(expected,-.15f,{0,0,1});
    expected=glm::translate(expected,-c.animated-glm::vec3(0,1.5,0));
    matrixNear(model::applyConstraint(m,c,.5),expected);
}

TEST_CASE("Constraint transition recovery uses listener initialValue, not a guessed zero coefficient") {
    auto clips=animation::parseAnimationFile(pack::parseJsonText(R"({"animations":{
        "a":{"animation_length":2,"bones":{"constraint":{"position":[0.1,0.1,0.1]}}},
        "b":{"animation_length":2,"bones":{"root":{"rotation":[0,0,0]}}}
    }})"));
    animation::Controller controller(clips);
    animation::Pose initial; initial["constraint"].translation=glm::vec3(2,3,4)/16.f;
    controller.setInitialPose(initial);
    controller.ensureTrackLineSize(1); controller.ensureTracksAmount(0,1); auto track=controller.getTrack(0,0);
    controller.runAnimation("a",track,false,animation::PlayType::Loop,0);
    controller.update(0);
    controller.runAnimation("b",track,false,animation::PlayType::Loop,1);
    auto pose=controller.update(.5);
    vectorNear(*pose["constraint"].translation*16.f,glm::vec3(.1f)*.0625f+glm::vec3(2,3,4)*.9375f);
}

TEST_CASE("Model-only projection correction: P_native*C equals model FOV, and optional isolated depth") {
    auto native=glm::perspective(glm::radians(70.f),16.f/9,.05f,1000.f);
    auto desired=model::withModelFov(native,45);
    auto correction=model::modelProjectionTransform(native,45);
    matrixNear(native*correction,desired);
    CHECK(correction[0][0] > 1); CHECK(correction[1][1] > 1);
    auto depth=native*model::modelProjectionTransform(native,45,2);
    CHECK(depth[2][2] == doctest::Approx(16.f/(.1f-16.f)));
    CHECK(depth[3][2] == doctest::Approx(16.f*.1f/(.1f-16.f)));
    auto near=depth*glm::vec4(0,0,-.1f,1), far=depth*glm::vec4(0,0,-16,1);
    CHECK(near.z/near.w == doctest::Approx(0).epsilon(1e-5));
    CHECK(far.z/far.w == doctest::Approx(1));
}

#ifdef TACZ_DEFAULT_PACK_DIR
TEST_CASE("Real AK47 functional channels: shoot has camera roll; idle and walks use static constraint layer") {
    auto packs=registry(); auto display=packs.gunDisplay({"tacz","ak47_display"}); REQUIRE(display);
    auto clips=pack::loadGunAnimations(packs,*display);
    animation::Pose shot;
    animation::evaluate(clips.at("shoot"),.0333f,shot);
    REQUIRE(shot["camera"].rotation);
    CHECK(glm::degrees(model::quaternionAngles(model::cameraBoneRotation(*shot["camera"].rotation))).z == doctest::Approx(-1.025f));
    for (auto name:{"static_idle","shoot","reload_tactical","reload_tactical_xmag","reload_empty","reload_empty_xmag"}) {
        auto const& channels=clips.at(name).channels;
        CHECK(std::any_of(channels.begin(),channels.end(),[](auto const& c){return c.bone=="constraint";}));
    }
    for (auto name:{"idle","walk_forward","walk_backward","walk_sideway"}) {
        auto const& channels=clips.at(name).channels;
        CHECK(std::none_of(channels.begin(),channels.end(),[](auto const& c){return c.bone=="constraint";}));
    }
    auto initial=model::functionalInitialPose(model::parseGeoModel(geometry()));
    vectorNear(*initial["constraint"].translation,glm::vec3(-.225f,-11.325f,-13.825f)/16.f);
}
TEST_CASE("Real AK47 ADS inverse: eye on origin, optical axis centred; no guessed translation") {
    auto model=model::parseGeoModel(geometry());
    auto idle=model::positioningInverse(model,model.path(model.find("idle_view")));
    auto iron=model::positioningInverse(model,model.path(model.find("iron_view")));
    auto placement=model::firstPersonAimTransform(idle,iron,1);
    auto projection=glm::perspective(glm::radians(45.f),16.f/9,.05f,1000.f);
    auto ndc=model::sightProjection(model,{},placement,projection);
    CHECK(ndc.x == doctest::Approx(0).epsilon(1e-5));
    CHECK(ndc.y == doctest::Approx(0).epsilon(1e-5));
    glm::mat4 path(1);
    for (auto i:model.path(model.find("iron_view"))) path*=model::partTransform(model.bones[i]);
    auto eye=placement*path*glm::vec4(0,0,0,1);
    vectorNear(eye,{0,0,0}); // cannot project this point: homogeneous w is zero
}

TEST_CASE("Real AK47: aimed reload keeps aim and same reload clip, constraint coefficients control pose") {
    auto packs=registry();
    auto display=packs.gunDisplay({"tacz","ak47_display"}); REQUIRE(display);
    auto clips=pack::loadGunAnimations(packs,*display);
    lua::AnimationContext context; context.ammo=12; context.barrel=true; context.maxAmmo=30;
    context.aiming=true; context.aimingProgress=1;
    lua::AnimationStateMachine machine(packs,clips,*display->stateMachine,context);
    REQUIRE(machine.initialize(0));
    REQUIRE(machine.trigger("draw"));
    for (int i=0;i<=120;++i) machine.update(i/60.0);
    machine.setTime(2.1); REQUIRE(machine.trigger("reload"));
    auto track=machine.controller().getTrack(0,9);
    CHECK(machine.controller().info(track).name == "reload_tactical");
    auto pose=machine.update(2.5);
    CHECK(context.aiming); CHECK(context.aimingProgress == 1);
    REQUIRE(pose.contains("constraint"));
    vectorNear(*pose["constraint"].translation*16.f,{.02,.02,.02});
    REQUIRE(pose.contains("camera"));
}

TEST_CASE("Real AK47 suspect cover/chamber cubes: independent TACZ helper pivot Rz Ry Rx and inflate vertices") {
    auto document=geometry();
    for (auto const& bone:document["minecraft:geometry"][0]["bones"]) {
        auto name=bone["name"].get<std::string>();
        std::vector<int> selected;
        if (name=="cover2") selected={0,12,13,22,23,24,25,26,27,28,37,39,42,43,44,48,54,55,58};
        if (name=="bolt") selected={0,2,3,4,12,15};
        for (int index:selected) {
            INFO(name," cube ",index);
            auto cube=bone["cubes"][index];
            auto fixture=document;
            auto part=bone; part.erase("parent"); part["cubes"]=nlohmann::json::array({cube});
            fixture["minecraft:geometry"][0]["bones"]=nlohmann::json::array({part});
            auto model=model::parseGeoModel(fixture);
            auto vertices=model::bake(model,[](auto const&,auto*){return model::Visit::Draw;});
            auto o=cube["origin"].get<std::array<double,3>>(), s=cube["size"].get<std::array<double,3>>();
            auto pivot=cube.value("pivot",bone["pivot"]).get<std::array<double,3>>();
            auto rot=cube.value("rotation",nlohmann::json::array({0,0,0})).get<std::array<double,3>>();
            double d=cube.value("inflate",0.0);
            std::vector<glm::vec3> expected;
            for (int corner=0;corner<8;++corner) {
                double x=o[0]-pivot[0]+((corner&1)?s[0]+d:-d);
                double y=pivot[1]-o[1]-s[1]+((corner&2)?s[1]+d:-d);
                double z=o[2]-pivot[2]+((corner&4)?s[2]+d:-d);
                auto rx=rot[0]*3.141592653589793/180, ry=rot[1]*3.141592653589793/180, rz=rot[2]*3.141592653589793/180;
                double yy=y*std::cos(rx)-z*std::sin(rx), zz=y*std::sin(rx)+z*std::cos(rx);
                double xx=x*std::cos(ry)+zz*std::sin(ry); zz=-x*std::sin(ry)+zz*std::cos(ry);
                x=xx*std::cos(rz)-yy*std::sin(rz); y=xx*std::sin(rz)+yy*std::cos(rz); z=zz;
                expected.emplace_back((x+pivot[0])/16,(y+24-pivot[1])/16,(z+pivot[2])/16);
            }
            CHECK(vertices.size() == cube["uv"].size()*4);
            for (auto const& v:vertices) {
                float nearest=1e9f;
                for (auto const& p:expected) nearest=std::min(nearest,glm::length(v.pos-p));
                CHECK(nearest < 2e-6f);
            }
            // Exact Java polygon order AND FaceUVsItem's mirrored east/west/up/down
            // lookups; no generic AABB corner-set test can catch a face permutation.
            std::array<std::string,6> faces{"west","east","up","down","north","south"};
            std::array<std::array<int,4>,6> corners{{{5,1,3,7},{0,4,6,2},{5,4,0,1},{3,2,6,7},{1,0,2,3},{4,5,7,6}}};
            size_t vertex=0;
            auto desc=fixture["minecraft:geometry"][0]["description"];
            for (size_t f=0;f<6;++f) if (cube["uv"].contains(faces[f])) {
                auto uv=cube["uv"][faces[f]]["uv"].get<std::array<float,2>>();
                auto size=cube["uv"][faces[f]]["uv_size"].get<std::array<float,2>>();
                std::array<glm::vec2,4> coords{{{uv[0]+size[0],uv[1]},{uv[0],uv[1]},
                    {uv[0],uv[1]+size[1]},{uv[0]+size[0],uv[1]+size[1]}}};
                for (int k=0;k<4;++k,++vertex) {
                    REQUIRE(vertex < vertices.size());
                    vectorNear(vertices[vertex].pos,expected[corners[f][k]]);
                    CHECK(vertices[vertex].uv.x == doctest::Approx(coords[k].x/desc["texture_width"].get<float>()));
                    CHECK(vertices[vertex].uv.y == doctest::Approx(coords[k].y/desc["texture_height"].get<float>()));
                }
            }
        }
    }
}
#endif
