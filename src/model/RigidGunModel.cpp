#include "model/RigidGunModel.h"
#include <algorithm>
#include <cmath>

namespace tacz::model {
namespace {
glm::mat3 normals(glm::mat4 const& m) {
    glm::mat3 n(m);
    if (std::abs(glm::determinant(n))>1e-12f) n=glm::transpose(glm::inverse(n));
    return n;
}
glm::vec3 normal(glm::mat3 const& matrix,glm::vec3 original) {
    auto n=matrix*original;
    return glm::dot(n,n)>1e-12f ? glm::normalize(n) : original;
}
void compile(GeoModel const& model,int index,int anchor,glm::mat4 relative,RigidGunModel& out,int offset=0) {
    auto const& bone=model.bones[index];
    (offset ? out.magazineTraversal : out.traversal).push_back(index);
    auto const* parent=bone.parent<0 ? nullptr : &model.bones[bone.parent];
    bool fold=anchor>=0 && bone.name.empty() && parent && parent->name!="attachment_adapter"
        && bone.illuminated==model.bones[anchor].illuminated;
    if (!fold) { anchor=index; relative=glm::mat4(1); }
    else relative*=out.restLocal[index];
    if (!bone.quads.empty()) {
        // Do not merge across intervening draw runs: preserve old DFS face order,
        // including equal-depth ties and helpers that follow named children.
        if (out.groups.empty() || out.groups.back().bone!=anchor+offset)
            out.groups.push_back({anchor+offset,model.bones[anchor].illuminated,{}});
        auto& vertices=out.groups.back().vertices;
        auto n=normals(relative);
        for (auto const& q:bone.quads) for (int k=0;k<4;++k)
            vertices.push_back({glm::vec3(relative*glm::vec4(q.pos[k]/16.f,1)),q.uv[k],normal(n,q.normal)});
    }
    for (int child:bone.children) compile(model,child,anchor,relative,out,offset);
    // BedrockGunModel.renderAdditionalMagazine: own geometry/children first, then
    // magazine.compile at this matrix (NOT magazine's animated local transform),
    // followed by its children with their normal local transforms and visibility.
    if (!offset && index==out.additionalMagazine && out.magazine>=0)
        compile(model,out.magazine,-1,glm::mat4(1),out,static_cast<int>(model.bones.size()));
}
}
RigidGunModel compileRigidGunModel(GeoModel const& model) {
    RigidGunModel result;
    result.magazine=model.find("magazine");
    result.additionalMagazine=model.find("additional_magazine");
    if (result.magazine>=0 && result.additionalMagazine>=0) {
        auto path=model.path(result.additionalMagazine);
        // Malformed packs must not recursively instance a subtree containing the target.
        if (std::find(path.begin(),path.end(),result.magazine)!=path.end()) result.magazine=-1;
    }
    result.restLocal.reserve(model.bones.size());
    for (auto const& bone:model.bones) result.restLocal.push_back(partTransform(bone));
    for (int root:model.roots) compile(model,root,-1,glm::mat4(1),result);
    return result;
}
bool rigidGunPoseCompatible(animation::Pose const& pose) { return !pose.contains(""); }
bool drawableRigidBone(BoneFrame const& frame,int bone) {
    if (bone<0 || static_cast<size_t>(bone)>=frame.world.size() || static_cast<size_t>(bone)>=frame.visible.size()) return false;
    if (!frame.visible[bone]) return false;
    auto const& m=frame.world[bone];
    for (int c=0;c<4;++c) for (int r=0;r<4;++r) if (!std::isfinite(m[c][r])) return false;
    // All-zero basis is an authored scale-zero hide (all faces have zero area).
    // Do not reject valid nonuniform/negative scales or merely planar geometry.
    return glm::vec3(m[0])!=glm::vec3(0) || glm::vec3(m[1])!=glm::vec3(0) || glm::vec3(m[2])!=glm::vec3(0);
}
void evaluateBoneFrame(GeoModel const& model,RigidGunModel const& rigid,
    animation::Pose const& pose,VisitFn const& visit,BoneFrame& frame) {
    auto count=model.bones.size()*(rigid.magazineTraversal.empty() ? 1 : 2);
    frame.world.resize(count,glm::mat4(1));
    frame.visible.resize(count);
    frame.parentVisible.resize(count);
    for (int index:rigid.traversal) {
        auto const& bone=model.bones[index];
        auto it=pose.find(bone.name);
        auto local=it==pose.end() ? rigid.restLocal[index] : partTransform(bone,&it->second);
        frame.world[index]=bone.parent<0 ? local : frame.world[bone.parent]*local;
        frame.parentVisible[index]=bone.parent<0 || frame.visible[bone.parent];
        frame.visible[index]=frame.parentVisible[index]
            && visit(bone,bone.parent<0 ? nullptr : &model.bones[bone.parent])==Visit::Draw;
    }
    int offset=static_cast<int>(model.bones.size());
    for (int index:rigid.magazineTraversal) {
        auto const& bone=model.bones[index];
        bool root=index==rigid.magazine;
        int parent=root ? rigid.additionalMagazine : offset+bone.parent;
        auto it=pose.find(bone.name);
        auto local=it==pose.end() ? rigid.restLocal[index] : partTransform(bone,&it->second);
        frame.world[offset+index]=root ? frame.world[parent] : frame.world[parent]*local;
        frame.parentVisible[offset+index]=frame.visible[parent];
        frame.visible[offset+index]=frame.visible[parent]
            && visit(bone,bone.parent<0 ? nullptr : &model.bones[bone.parent])==Visit::Draw;
    }
}
std::vector<Vertex> flattenRigidGunModel(RigidGunModel const& rigid,BoneFrame const& frame) {
    std::vector<Vertex> result;
    for (auto const& group:rigid.groups) if (frame.visible[group.bone]) {
        auto const& m=frame.world[group.bone]; auto n=normals(m);
        for (auto const& v:group.vertices) result.push_back({glm::vec3(m*glm::vec4(v.pos,1)),v.uv,normal(n,v.normal)});
    }
    return result;
}
} // namespace tacz::model
