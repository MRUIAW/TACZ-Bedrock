#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include "model/ViewTurnInertia.h"
#include "model/FunctionalBones.h"
#include "model/PlayerArms.h"
#include "model/RigidGunModel.h"

using namespace tacz;
namespace {
void near(glm::mat4 const& a, glm::mat4 const& b, double eps=2e-6) {
    for (int c=0;c<4;++c) for (int r=0;r<4;++r) CHECK(a[c][r]==doctest::Approx(b[c][r]).epsilon(eps));
}
model::GeoModel fixture() {
    return model::parseGeoModel(nlohmann::json::parse(R"({"format_version":"1.12.0","minecraft:geometry":[{
        "description":{"identifier":"geometry.synthetic","texture_width":16,"texture_height":16},"bones":[
        {"name":"root","pivot":[2,24,3],"rotation":[12,-23,34],"cubes":[{"origin":[0,23,0],"size":[1,2,3],"uv":[0,0]}]},
        {"name":"righthand_pos","parent":"root","pivot":[4,25,6],"rotation":[-8,13,19]},
        {"name":"constraint","parent":"root","pivot":[1,26,7]},
        {"name":"iron_view","parent":"root","pivot":[2,27,4]}]}]})"));
}
}
TEST_CASE("Turn bob copies previous before fixed 0.5 convergence and interpolates view minus bob") {
    model::ViewTurnBob bob;
    bob.initialize(10,20);
    bob.tick(30,60);
    CHECK(bob.xBobO==10); CHECK(bob.yBobO==20);
    CHECK(bob.xBob==20); CHECK(bob.yBob==40);
    auto f=bob.frame(30,60,.25f);
    CHECK(f.dx==17.5); CHECK(f.dy==35);
    bob.tick(30,60);
    CHECK(bob.xBobO==20); CHECK(bob.yBobO==40);
    CHECK(bob.xBob==25); CHECK(bob.yBob==50);
    for (int i=0;i<30;++i) bob.tick(30,60);
    CHECK(std::abs(bob.frame(30,60,1).dx)<1e-7);
    CHECK(std::abs(bob.frame(30,60,1).dy)<1e-7);
}
TEST_CASE("Turn bob 2a initialization and lifecycle reinitialization have no opening offset") {
    model::ViewTurnBob bob;
    for (auto view:{glm::dvec2(67,-179),glm::dvec2(-53,172),glm::dvec2(12,630)}) {
        bob.initialize(view.x,view.y); // world entry / player replacement / respawn
        for (float alpha:{0.f,.37f,1.f}) {
            auto f=bob.frame(view.x,view.y,alpha);
            CHECK(f.dx==0); CHECK(f.dy==0); CHECK(f.sx==0); CHECK(f.sy==0);
        }
    }
    bob.tick(100,700);
    bob.initialize(25,-42);
    CHECK(bob.frame(25,-42,.5f)==model::ViewTurnFrame{});
}
TEST_CASE("Turn bob is frozen for dead sleeping and off-camera ticks, without teleport heuristics") {
    model::ViewTurnBob bob;
    bob.initialize(0,0); bob.tick(20,40);
    for (int i=0;i<3;++i) { // adapter supplies false for each Java gate
        bob.tick(60,80,false);
        CHECK(bob.xBobO==0); CHECK(bob.xBob==10);
        CHECK(bob.yBobO==0); CHECK(bob.yBob==20);
    }
    // A teleport with changed orientation is an ordinary observation, not a reset.
    bob.tick(-70,-90);
    CHECK(bob.xBobO==10); CHECK(bob.xBob==-30);
    CHECK(bob.frame(-70,-90,1).dx==-40);
}
TEST_CASE("Turn inertia preserves signs and float tanh oddness and limits") {
    for (double d:{.1,1.,25.,90.,1000.}) {
        auto p=model::viewTurnFrame(d,d),n=model::viewTurnFrame(-d,-d);
        CHECK(p.sx==static_cast<float>(std::tanh(d/25.))*25.f);
        CHECK(p.sx>0); CHECK(p.sy>0); CHECK(n.sx==-p.sx); CHECK(n.sy==-p.sy);
        CHECK(p.sx<=25); CHECK(p.sy<=25);
    }
    CHECK(model::viewTurnFrame(1e20,-1e20).sx==25);
    CHECK(model::viewTurnFrame(1e20,-1e20).sy==-25);
    auto geo=fixture();
    auto pitch=model::effectiveViewTurnPose(geo,{},model::viewTurnFrame(25,0)).at("root");
    CHECK(pitch.translation->y>0); CHECK(pitch.translation->x==0);
    CHECK(pitch.rotation->x>0);
    auto yaw=model::effectiveViewTurnPose(geo,{},model::viewTurnFrame(0,-25)).at("root");
    CHECK(yaw.translation->x<0); CHECK(yaw.translation->y==0); CHECK(yaw.rotation->y<0);
}
TEST_CASE("Turn root quaternion postcomposition matches explicit rest animation inertia scale matrix") {
    auto geo=fixture();
    animation::Pose animation;
    auto& root=animation["root"];
    root.translation=glm::vec3(.13f,-.21f,.07f);
    root.rotation=glm::radians(glm::vec3(21,-32,43));
    root.scale=glm::vec3(1.2f,.7f,1.9f);
    auto saved=animation;
    auto turn=model::viewTurnFrame(35,-47);
    auto effective=model::effectiveViewTurnPose(geo,animation,turn);
    auto const& bone=geo.bones[geo.find("root")];
    auto offset=*root.translation+glm::vec3(turn.sy*.1f/48.f,turn.sx*.1f/48.f,0);
    auto expected=glm::translate(glm::mat4(1),offset*glm::vec3(1,-1,1)+bone.pivot/16.f);
    for (int i:{2,1,0}) { glm::vec3 axis(0); axis[i]=1; expected=glm::rotate(expected,bone.rotation[i],axis); }
    for (int i:{2,1,0}) { glm::vec3 axis(0); axis[i]=1; expected=glm::rotate(expected,(*root.rotation)[i],axis); }
    expected=glm::rotate(expected,glm::radians(turn.sx*.05f),{1,0,0});
    expected=glm::rotate(expected,glm::radians(turn.sy*.05f),{0,1,0});
    expected=glm::scale(expected,*root.scale);
    near(model::partTransform(bone,&effective.at("root")),expected);
    for (int i=0;i<100;++i) CHECK(model::effectiveViewTurnPose(geo,animation,turn)==effective);
    CHECK(animation==saved);
    auto noRoot=geo; noRoot.bones[0].name="other";
    noRoot.byName.erase("root"); noRoot.byName.emplace("other",0);
    CHECK(model::effectiveViewTurnPose(noRoot,animation,turn)==animation); // named root only
}
TEST_CASE("Turn residual is single-axis identity and the raw diagonal commutator") {
    for (double d:{-150.,-25.,0.,25.,150.}) {
        near(model::viewTurnResidual(model::viewTurnFrame(d,0)),glm::mat4(1));
        near(model::viewTurnResidual(model::viewTurnFrame(0,d)),glm::mat4(1));
    }
    for (auto d:{glm::vec2(25,25),glm::vec2(-40,70),glm::vec2(130,-89)}) {
        auto x=glm::rotate(glm::mat4(1),glm::radians(d.x*.1f),{1,0,0});
        auto y=glm::rotate(glm::mat4(1),glm::radians(d.y*.1f),{0,1,0});
        near(model::viewTurnResidual(model::viewTurnFrame(d.x,d.y)),x*y*glm::transpose(x)*glm::transpose(y));
    }
    CHECK(model::viewTurnResidualDegrees(model::viewTurnResidual(model::viewTurnFrame(25,25)))
        ==doctest::Approx(.1090658).epsilon(.001));
    // Axis/sign basis: at any proper captured camera basis B, local +X/+Y are
    // still Java's right/up axes. There is no model Y reflection before C.
    auto base=glm::rotate(glm::mat4(1),.61f,{0,1,0});
    auto x=glm::rotate(glm::mat4(1),.2f,{1,0,0});
    CHECK((x*glm::vec4(0,1,0,0)).z>0);
    near(base*x,glm::rotate(base,.2f,{1,0,0}));
}
TEST_CASE("Turn residual follows the camera quaternion and precedes placement, not C H placement") {
    auto h=glm::mat4_cast(glm::quat(glm::radians(glm::vec3(21,-17,13))));
    auto placement=glm::translate(glm::mat4(1),{.4f,-.2f,-1.1f});
    placement=glm::rotate(placement,.7f,{0,0,1});
    auto turn=model::viewTurnFrame(75,-63);
    auto c=model::viewTurnResidual(turn);
    auto composed=h*c*placement;
    auto x=glm::rotate(glm::mat4(1),glm::radians(7.5f),{1,0,0});
    auto y=glm::rotate(glm::mat4(1),glm::radians(-6.3f),{0,1,0});
    near(composed,h*x*y*glm::transpose(x)*glm::transpose(y)*placement);
    float difference=0;
    auto wrong=c*h*placement;
    for (int col=0;col<4;++col) for (int row=0;row<4;++row)
        difference=std::max(difference,std::abs(composed[col][row]-wrong[col][row]));
    CHECK(difference>1e-4f);
    // The existing projective adapter remains on the left, before H and C.
    auto native=glm::mat4(1),modelProjection=native;
    modelProjection[0][0]=1.4f; modelProjection[1][1]=1.4f;
    auto correction=glm::inverse(native)*modelProjection;
    near(native*correction*h*c*placement,modelProjection*composed);
}
TEST_CASE("Turn passenger view yaw can interpolate head while bob remains body-yaw driven") {
    model::ViewTurnBob bob; bob.initialize(10,20);
    bob.tick(30,40);
    double headOld=50,headCurrent=70;
    auto riding=bob.frame(30,std::lerp(headOld,headCurrent,.25),.25f);
    CHECK(riding.dx==17.5);
    CHECK(riding.dy==32.5); // 55 - lerp(20,30,.25), not body-current 40
    CHECK(bob.frame(30,40,.25f).dy==17.5);
    CHECK(bob.yBob==30); CHECK(bob.yBobO==20);
}
TEST_CASE("Turn yaw unwrap crosses boundaries and common rebasing never wraps view minus bob") {
    model::ViewTurnBob bob; bob.initialize(0,179);
    bob.tick(0,-179);
    CHECK(bob.yaw==-179); CHECK(bob.yBobO==-181); CHECK(bob.yBob==-180);
    CHECK(bob.frame(0,-178,.5f).dy==2.5);
    bob.initialize(0,-179); bob.tick(0,179);
    CHECK(bob.frame(0,179,1).dy==-1);
    bob.initialize(0,0);
    for (int i=1;i<=6;++i) bob.tick(0,120.*i,false);
    CHECK(bob.frame(0,720,1).dy==720); // intentionally NOT shortest(view-bob)
    for (int i=0;i<10000;++i) bob.tick(0,720.+120.*i);
    CHECK(std::abs(bob.yaw)<=180);
    CHECK(std::isfinite(bob.yBob));
    CHECK(model::shortestTurn(1e300,-1e300)>=-180);
    CHECK(model::shortestTurn(1e300,-1e300)<180);
    bob.initialize(1e30,1e30); bob.tick(-1e30,-1e30);
    auto f=bob.frame(-1e30,-1e30,.5f);
    CHECK(std::isfinite(f.dx)); CHECK(std::isfinite(f.dy));
    CHECK(std::isfinite(model::viewTurnResidualDegrees(model::viewTurnResidual(f))));
}
TEST_CASE("Turn nonfinite input retires the state and next valid tick initializes cleanly") {
    model::ViewTurnBob bob; bob.initialize(10,20);
    bob.tick(std::numeric_limits<double>::quiet_NaN(),40);
    CHECK_FALSE(bob.initialized);
    CHECK(bob.frame(1,2,.5f)==model::ViewTurnFrame{});
    bob.tick(30,40);
    CHECK(bob.initialized); CHECK(bob.frame(30,40,.5f)==model::ViewTurnFrame{});
    bob.tick(30,std::numeric_limits<double>::infinity());
    CHECK_FALSE(bob.initialized);
}
TEST_CASE("Turn capture rejects scope world client player camera stack top and depth reuse") {
    model::HandTurnCaptureKey key{1,2,3,4,5,6,7,8};
    CHECK(model::validHandTurnCapture(key,key));
    for (int i=0;i<8;++i) {
        auto changed=key;
        switch (i) {
        case 0: ++changed.scope; break; case 1: ++changed.world; break;
        case 2: ++changed.client; break; case 3: ++changed.player; break;
        case 4: ++changed.camera; break; case 5: ++changed.stack; break;
        case 6: ++changed.top; break; case 7: ++changed.depth; break;
        }
        CHECK_FALSE(model::validHandTurnCapture(key,changed));
    }
    CHECK_FALSE(model::validHandTurnCapture({},{}));
    auto retired=key; retired.world=0;
    CHECK_FALSE(model::validHandTurnCapture(retired,retired));
    // Simulated PRE-push selection only changes the new child, never its parent.
    auto parent=glm::translate(glm::mat4(1),{1,2,3}),saved=parent;
    auto captured=glm::rotate(glm::mat4(1),.3f,{0,1,0});
    auto c=model::viewTurnResidual(model::viewTurnFrame(25,25));
    auto placement=glm::translate(glm::mat4(1),{4,5,6});
    auto child=(model::validHandTurnCapture(key,key) ? captured : parent)*c*placement;
    near(child,captured*c*placement); near(parent,saved);
    child=(model::validHandTurnCapture(key,retired) ? captured : parent)*c*placement;
    near(child,parent*c*placement); near(parent,saved);
}
TEST_CASE("Turn effective root feeds rigid CPU constraint sight and attached arm paths consistently") {
    auto geo=fixture(); auto rigid=model::compileRigidGunModel(geo);
    auto animation=model::functionalInitialPose(geo);
    animation["root"].rotation=glm::vec3(.2f,-.1f,.3f);
    auto effective=model::effectiveViewTurnPose(geo,animation,model::viewTurnFrame(30,-40));
    model::BoneFrame frame;
    auto visible=[](auto const&,auto const*) { return model::Visit::Draw; };
    model::evaluateBoneFrame(geo,rigid,effective,visible,frame);
    auto cpu=model::bake(geo,visible,effective);
    auto flattened=model::flattenRigidGunModel(rigid,frame);
    REQUIRE(cpu.size()==flattened.size());
    for (size_t i=0;i<cpu.size();++i) for (int axis=0;axis<3;++axis)
        CHECK(cpu[i].pos[axis]==doctest::Approx(flattened[i].pos[axis]).epsilon(2e-6));
    auto path=geo.path(geo.find("righthand_pos"));
    auto arm=model::playerArmTransform(geo,path,effective);
    auto rigidArm=model::playerArmTransformFromFrame(geo,path,frame);
    REQUIRE(arm); REQUIRE(rigidArm); near(*arm,*rigidArm);
    CHECK(*arm!=*model::playerArmTransform(geo,path,animation));
    auto constraint=model::constraintTransform(geo,effective);
    auto old=model::constraintTransform(geo,animation);
    CHECK(constraint.animated!=old.animated); CHECK(constraint.rotation!=old.rotation);
    auto projection=glm::mat4(1);
    CHECK(model::sightProjection(geo,effective,glm::mat4(1),projection)
        !=model::sightProjection(geo,animation,glm::mat4(1),projection));
}
