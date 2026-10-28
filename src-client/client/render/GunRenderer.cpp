#include "client/render/GunRenderer.h"

#include <exception>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <type_traits>

#include <glm/glm.hpp>
#include <stb_image.h>

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/IClientInstance.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/common/Globals.h"
#include "mc/deps/minecraft_renderer/framebuilder/FrameBuilder.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/client/renderer/RenderMaterialGroup.h"
#include "mc/client/renderer/SupplementaryFieldAutoGenerationMode.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/deps/core/container/Blob.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/image/Image.h"
#include "mc/deps/core/image/ImageFormat.h"
#include "mc/deps/core/image/ImageUsage.h"
#include "mc/deps/core/math/AlphaUsage.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/resource/ResourceFileSystem.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core/renderer/RenderMaterialInfo.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/core_graphics/TextureSetLayerType.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/deps/minecraft_renderer/game/ItemContextFlags.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTexture.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/RenderMaterial.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/MeshData.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/renderer/TextureContainer.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "mc/deps/nbt/CompoundTagVariant.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/item/ItemStack.h"

#include "mc/client/renderer/ActorShaderManager.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/ptexture/LightTexture.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec4.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/BrightnessPair.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include <fmt/format.h>
#include <glm/gtc/matrix_transform.hpp>

#include "debug/RenderDebug.h"
#include "client/animation/GunAnimation.h"
#include "client/gameplay/ClientShooter.h"
#include "client/gameplay/ClientAim.h"
#include "client/render/PlayerArms.h"
#include "client/render/SkinTextureSource.h"
#include "model/GeoModel.h"
#include "model/GunModelRules.h"
#include "model/RenderResourceKey.h"
#include "model/Aiming.h"
#include "model/FunctionalBones.h"
#include "model/MeshDiagnostics.h"
#include "model/RigidGunModel.h"
#include "model/PackedUV.h"
#include "model/LocalMeshFrame.h"
#include <glm/gtc/packing.hpp>
#include <cstring>
#include "model/GunAlpha.h"
#include "client/render/CameraBones.h"
#include "mod/TaczMod.h"
#include "nbt/GunNbt.h"
#include "pack/GunPackParser.h"

namespace tacz::client::render {
namespace {

using TextureVariant = std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture>;

using FirstPersonSignature = void (ItemInHandRenderer::*)(BaseActorRenderContext&, Matrix const&, ItemContextFlags);
static_assert(std::is_same_v<decltype(&ItemInHandRenderer::renderFirstPerson), FirstPersonSignature>);
using MeshRenderSignature=void (mce::Mesh::*)(mce::MeshContext&,mce::MaterialPtr const&,TextureVariant const&,
    uint,uint,OffscreenCaptureDescription const&,mce::IndexBufferContainer const*) const;
static_assert(std::is_same_v<decltype(static_cast<MeshRenderSignature>(&mce::Mesh::renderMesh)),MeshRenderSignature>);
static_assert(std::is_same_v<decltype(&Tessellator::vertex),void (Tessellator::*)(float,float,float)>);
static_assert(std::is_same_v<decltype(&MatrixStack::push),MatrixStack::MatrixStackRef (MatrixStack::*)(bool)>);
using UploadContainer=BedrockTexture& (mce::TextureGroup::*)(ResourceLocation const&,mce::TextureContainer&&,std::string_view);
static_assert(std::is_same_v<decltype(static_cast<UploadContainer>(&mce::TextureGroup::uploadTexture)),UploadContainer>);
static_assert(std::is_constructible_v<mce::TextureContainer,cg::ImageBuffer&&,mce::AllowDroppingMips>);

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

// Everything needed to draw one gun display, built on first use on the render thread.
struct GunVisual {
    struct GroupMesh { std::optional<mce::Mesh> mesh; uint count=0; glm::vec3 origin{0}; };
    bool                              failed = false; // logged once; retry after resource-scope refresh
    glm::mat4                         transform{1.0f};
    glm::mat4                         idleView{1}, ironView{1};
    model::AimDynamics                modelFov{.5f, 1.2f, .5f};
    std::optional<std::chrono::steady_clock::time_point> modelFovAt;
    bool frozen = false;
    int capturedDepthMode = 0;
    bool loggedAimProjection = false;
    std::vector<model::Vertex>        vertices; // baked rest pose, kept to rebuild the mesh
    model::GeoModel                   model;
    model::RigidGunModel              rigid;
    model::BoneFrame                  frame;
    uint64_t                          frameRevision=0;
    std::array<std::vector<std::unique_ptr<GroupMesh>>,2> rigidMeshes;
    std::array<bool,2>                 rigidReady{};
    bool                              warnedIncompatible=false;
    bool                              loggedInvalidMesh=false;
    uint64_t                          localHash=0;
    bool                              enableTransparency=false;
    bool                              warnedBlendFallback=false;
    std::vector<model::AlphaClass>     alphaClasses;
    std::vector<size_t>               opaqueGroups,alphaGroups;
    ArmPaths                          armPaths;
    uint64_t                          animationRevision = 0;
    size_t                            measuredFrames = 0;
    double                            measuredMs = 0;
    double                            maxMs = 0;
    std::optional<mce::Mesh>          mesh;
    bool                              meshTwoSided = false;
    uint                              vertexCount  = 0;
    struct Diagnostics {
        std::vector<model::Vertex> previous;
        glm::mat4 previousClip{1};
        bool hasClip=false;
        uint64_t hash=0, previousHash=0, rawHash=0;
        size_t frames=0, changes=0, topologyChanges=0, rawBytes=0;
        float maxDelta=0, maxClipDelta=0, uploadDelta=-1;
        int revision=-1;
        std::vector<glm::mat4> previousBones;
        std::chrono::steady_clock::time_point at=std::chrono::steady_clock::now();
    } diagnostics;
    std::unique_ptr<ResourceLocation> textureLocation;
    std::unique_ptr<ResourceLocation> cutoutLocation;
    TextureVariant                    texture;
    TextureVariant                    cutoutTexture;
    std::weak_ptr<mce::TextureGroup>   textureOwner;
};

// Render diagnostics (/tacz rdebug). Materials:
//   0 entity_alphatest   1 renderer entity    2 renderer static
//   3 renderer alpha block   4 debug (vertex colour)   5 holo_hand_pointer
//   6 moving-block alphatest (what Lamium uses for textured ghost blocks)
// Verified in game (0.2.2): 4 and 6 draw; 2 is not populated on ItemInHandRenderer.
constexpr int MaterialCount = 8;

char const* materialName(int index) {
    static constexpr char const* names[MaterialCount] = {
        "entity_alphatest",
        "renderer entity",
        "renderer static",
        "renderer alpha block",
        "debug",
        "holo_hand_pointer",
        "moving-block alphatest",
        "moving-block blend (native pipeline variant)",
    };
    return index >= 0 && index < MaterialCount ? names[index] : "?";
}

std::mutex cacheMutex;

// Meshes, textures and materials are GPU resources. They are intentionally leaked at process
// exit: a static destructor could run after the renderer is gone. disable() empties them.
struct VisualKey {
    pack::ResourceId display,model,texture;
    auto operator<=>(VisualKey const&) const = default;
};
auto& cache     = *new std::map<VisualKey, GunVisual>;
auto& materials = *new std::array<std::optional<mce::MaterialPtr>, MaterialCount>;
int   loggedRevision = -1;
std::optional<model::RenderResourceKey> resourceKey;
uint64_t resourceSerial = 0;
uint64_t textureSerial = 0;
int boundSampler=-1;
struct RenderCounters {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    uint64_t hooks = 0, builds = 0, draws = 0, poseChanges = 0, invalidMeshes = 0;
    uint64_t lastPose = 0, lastMesh = 0, animationFrame = 0;
    int path=-1;
    uint64_t cacheHits=0,cacheMisses=0,frames=0,armDraws=0;
    double totalMs=0,maxMs=0,prepareMs=0,drawMs=0;
    size_t warmFrames=0;
    double warmMs=0,warmMax=0,warmPrepare=0,warmDraw=0;
} counters;
bool waitingForResources = false;

// Caller holds cacheMutex. TexturePtr is released before asking its ORIGINAL group to
// unload our Raw resource. Never query a new world's group to release an old name.
void releaseResources(char const* reason) {
    releasePlayerArms(reason);
    for (auto& [id, visual] : cache) {
        visual.mesh.reset();
        for (auto& variant:visual.rigidMeshes) variant.clear();
        visual.texture = {};
        visual.cutoutTexture={};
        if (visual.cutoutLocation) {
            auto owner=visual.textureOwner.lock();
            if (owner) owner->unloadTexture(*visual.cutoutLocation,false);
        }
        if (visual.textureLocation) {
            auto owner = visual.textureOwner.lock();
            bool unloaded = owner && owner->unloadTexture(*visual.textureLocation, false);
            logger().info("[gun-gpu] release {} world={} serial={} reason={} textureUnloaded={}", id.display.toString(),
                resourceKey ? resourceKey->world : 0, resourceSerial, reason, unloaded);
        }
    }
    cache.clear();
    for (auto& slot : materials) slot.reset();
    resourceKey.reset();
    loggedRevision = -1;
    counters = {};
    waitingForResources = false;
}

BedrockTexture& uploadGunImage(mce::TextureGroup& group,ResourceLocation const& location,mce::Image&& image) {
    cg::ImageBuffer buffer(std::move(image)); // one decoded image, no mip chain
    if (boundSampler==0) return group.uploadTexture(location,std::move(buffer));
    mce::TextureContainer container(std::move(buffer),static_cast<mce::AllowDroppingMips>(0));
    auto previousMips=container.mDescription->mMipCount;
    container.mDescription->mMipCount=1;
    auto& texture=group.uploadTexture(location,std::move(container),"tacz single-mip gun");
    std::shared_ptr<BedrockTextureData> data=texture.mBedrockTextureData;
    logger().info("[gun-sampling] upload={} mode=1 inputMips={} requestedMips=1 nativeMetadataMips={} (filter/address are material-owned; GPU sampler not inferred)",
        resourceSerial,previousMips,data ? data->mTextureDescription->mMipCount : 0);
    return texture;
}
// Decodes a pack PNG and uploads it under a Raw (in-memory) resource location.
bool uploadTexture(IClientInstance& client, std::string const& png, std::string const& name, GunVisual& visual) {
    int   w = 0, h = 0, channels = 0;
    auto* pixels = stbi_load_from_memory(
        reinterpret_cast<stbi_uc const*>(png.data()),
        static_cast<int>(png.size()),
        &w,
        &h,
        &channels,
        4
    );
    if (!pixels) {
        logger().warn("Texture {} is not a valid PNG: {}", name, stbi_failure_reason());
        return false;
    }
    std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> decoded(pixels,stbi_image_free);
    auto rgba=std::span<uint8_t const>(pixels,static_cast<size_t>(w)*h*4);
    for (size_t i=0;i<visual.rigid.groups.size();++i) {
        auto kind=model::groupAlpha(visual.rigid.groups[i].vertices,rgba,w,h);
        visual.alphaClasses.push_back(kind);
        (kind==model::AlphaClass::Opaque ? visual.opaqueGroups : visual.alphaGroups).push_back(i);
    }
    mce::Image image(static_cast<uint32>(w), static_cast<uint32>(h), mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::SRGB);
    image.mAlphaUsage = mce::AlphaUsage::Transparent;
    image.setRawImage(mce::Blob(pixels, static_cast<size_t>(w) * static_cast<size_t>(h) * 4));

    auto group = client.getTextureGroup();
    if (!group) return false;
    auto runtimeName = fmt::format("tacz/runtime/{}/{}/{}/{}", resourceKey->world, resourceSerial, ++textureSerial, name);
    visual.textureLocation = std::make_unique<ResourceLocation>(Core::PathView(runtimeName), ResourceFileSystem::Raw);
    auto& texture          = uploadGunImage(*group,*visual.textureLocation,std::move(image));
    visual.texture         = mce::TexturePtr(texture, *visual.textureLocation, cg::TextureSetLayerType::Color);
    visual.textureOwner = group;
    bool fractional=std::find(visual.alphaClasses.begin(),visual.alphaClasses.end(),model::AlphaClass::Fractional)!=visual.alphaClasses.end();
    if (fractional) {
        std::vector<uint8_t> mask(rgba.begin(),rgba.end());
        model::javaCutoutMask(mask);
        mce::Image cutout(static_cast<uint32>(w),static_cast<uint32>(h),mce::ImageFormat::RGBA8Unorm,mce::ImageUsage::SRGB);
        cutout.mAlphaUsage=mce::AlphaUsage::Transparent;
        cutout.setRawImage(mce::Blob(mask.data(),mask.size()));
        auto maskName=runtimeName+"/java-cutout";
        visual.cutoutLocation=std::make_unique<ResourceLocation>(Core::PathView(maskName),ResourceFileSystem::Raw);
        auto& masked=uploadGunImage(*group,*visual.cutoutLocation,std::move(cutout));
        visual.cutoutTexture=mce::TexturePtr(masked,*visual.cutoutLocation,cg::TextureSetLayerType::Color);
    } else visual.cutoutTexture=visual.texture;
    logger().info("[gun-alpha] {} displayTransparency={} fullyOpaqueGroups={} alphaGroups={} fractionalGroups={} JavaCutoutThreshold=.1 maskUploaded={} (backend filtering/RTX acceptance pending)",
        name,visual.enableTransparency,visual.opaqueGroups.size(),visual.alphaGroups.size(),
        std::count(visual.alphaClasses.begin(),visual.alphaClasses.end(),model::AlphaClass::Fractional),fractional);
    logger().info("[gun-gpu] create {} ({}x{}) world={} serial={} group={}", name, w, h,
        resourceKey->world, resourceSerial, fmt::ptr(group.get()));
    return true;
}

// Baked vertices -> buffered quad mesh. With `twoSided` every quad is emitted a second time
// with reversed winding, so face culling cannot hide the model.
void inspectCachedMesh(mce::Mesh const& mesh,int bone,std::string const& name,glm::vec3 origin) {
    std::array<unsigned char,110> layout{};
    std::memcpy(layout.data(),&mesh.mVertexLayout,layout.size());
    auto word=[&](size_t at) { uint16_t v; std::memcpy(&v,layout.data()+at,2); return v; };
    size_t stride=word(0x20),position=word(2),end=stride;
    for (size_t f=1;f<15;++f) if ((word(0)&(1u<<f)) && word(2+2*f)>position) end=std::min<size_t>(end,word(2+2*f));
    size_t slot=end>=position ? end-position : 0;
    auto const& raw=mesh.mRawData.get();
    logger().info("[gun-packed-cache] bone={} name={} positionSlotBytes={} stride={} split={} rawBytes={} origin=({},{},{})",
        bone,name,slot,stride,layout[0x6c],raw.size(),origin.x,origin.y,origin.z);
    if (layout[0x6c] || (slot!=8 && slot!=12) || !stride || position+slot>stride) return;
    for (size_t i=0;i<4 && raw.size()>=stride*(i+1);++i) {
        glm::vec3 p;
        for (size_t axis=0;axis<3;++axis) {
            if (slot==8) { uint16_t h; std::memcpy(&h,raw.data()+stride*i+position+axis*2,2); p[axis]=glm::unpackHalf1x16(h); }
            else std::memcpy(&p[axis],raw.data()+stride*i+position+axis*4,4);
        }
        logger().info("[gun-packed-position] bone={} i={} packedLocal=({},{},{}) restoredLocal=({},{},{}) format={}",
            bone,i,p.x,p.y,p.z,p.x+origin.x,p.y+origin.y,p.z+origin.z,slot==8 ? "half4" : "float3");
    }
}
mce::Mesh uploadMesh(ScreenContext& screen, std::span<model::Vertex const> vertices, bool twoSided,uint& uploadedCount,float* uploadDelta=nullptr) {
    // Gun-only upload/cache boundary; Java UVs stay untouched in GeoModel and
    // rigid groups. Arms have a separate uploader. No per-frame default rebuild.
    auto safeVertices=model::packedRepeatUVQuads(vertices);
    vertices=safeVertices;
    uploadedCount=static_cast<uint>(vertices.size()*(twoSided ? 2 : 1));
    Tessellator batch(screen.tessellator.mBufferResourceService);
    batch.begin({}, mce::PrimitiveMode::QuadList, static_cast<int>(vertices.size() * (twoSided ? 2 : 1)), false);
    batch.color(1.f, 1.f, 1.f, 1.f);
    auto emit = [&](model::Vertex const& v, float normalSign) {
        batch.mNextUV[0]  = v.uv;
        batch.mNextNormal = glm::vec4(v.normal * normalSign, 0.f);
        batch.vertex(v.pos.x, v.pos.y, v.pos.z);
    };
    for (size_t q = 0; q + 4 <= vertices.size(); q += 4) {
        for (size_t k = 0; k < 4; ++k) emit(vertices[q + k], 1.f);
        if (twoSided) {
            for (size_t k = 4; k-- > 0;) emit(vertices[q + k], -1.f);
        }
    }
    auto const& data = batch.mMeshData.get();
    if (uploadDelta) *uploadDelta=-1;
    if (debug::meshDiagnostics && uploadDelta) {
        auto const& positions=data.mPositions.get();
        size_t expected=vertices.size()*(twoSided ? 2 : 1);
        *uploadDelta=positions.size()==expected ? 0.f : -1.f;
        if (*uploadDelta==0) for (size_t i=0;i<positions.size();++i) {
            size_t quad=i/(twoSided ? 8 : 4), corner=i%(twoSided ? 8 : 4);
            size_t source=quad*4+(corner<4 ? corner : 7-corner);
            for (int axis=0;axis<3;++axis) *uploadDelta=std::max(*uploadDelta,std::abs(positions[i][axis]-vertices[source].pos[axis]));
        }
    }
    auto mesh=batch.end(Tessellator::UploadMode::Buffered, "tacz gun", SupplementaryFieldAutoGenerationMode{});
    if (debug::meshDiagnostics) {
        // Read-only 1.26.51 layout evidence; never construct/mutate the SDK's
        // empty VertexLayout placeholder. Bounds-check before inspecting raw bytes.
        std::array<unsigned char,110> layout{};
        std::memcpy(layout.data(),&mesh.mVertexLayout,layout.size());
        auto word=[&](size_t at) { uint16_t value; std::memcpy(&value,layout.data()+at,2); return value; };
        auto const& raw=mesh.mRawData.get();
        size_t offset=word(0xa),stride=word(0x20);
        size_t positionEnd=stride,positionOffset=word(2);
        for (size_t field=1;field<15;++field) if ((word(0)&(1u<<field)) && word(2+2*field)>positionOffset)
            positionEnd=std::min<size_t>(positionEnd,word(2+2*field));
        logger().info("[gun-packed] rawBytes={} stride={} UV0offset={} positionSlotBytes={} splitStream={} sourceVertices={} uploadedVertices={} (read-only CPU packed bytes, not GPU capture)",
            raw.size(),stride,offset,positionEnd>=positionOffset ? positionEnd-positionOffset : 0,layout[0x6c],safeVertices.size(),uploadedCount);
        if (!layout[0x6c] && (word(0)&0x10) && offset+4<=stride && stride && raw.size()>=stride*std::min<size_t>(8,uploadedCount)) {
            for (size_t i=0;i<std::min<size_t>(4,safeVertices.size());++i) {
                uint16_t u,v;
                std::memcpy(&u,raw.data()+stride*i+offset,2); std::memcpy(&v,raw.data()+stride*i+offset+2,2);
                logger().info("[gun-packed-UV] i={} CPU=({}, {}) words=({}, {}) expected=({}, {}) decode=({}, {})",
                    i,safeVertices[i].uv.x,safeVertices[i].uv.y,u,v,model::packUV16(safeVertices[i].uv.x),model::packUV16(safeVertices[i].uv.y),u/65535.,v/65535.);
            }
        }
    }
    ++counters.builds;
    return mesh;
}
void buildMesh(ScreenContext& screen,GunVisual& visual,bool twoSided) {
    visual.meshTwoSided=twoSided;
    visual.mesh.emplace(uploadMesh(screen,visual.vertices,twoSided,visual.vertexCount,&visual.diagnostics.uploadDelta));
}
void ensureRigidMeshes(ScreenContext& screen,GunVisual& visual,bool twoSided) {
    size_t variant=twoSided ? 1 : 0;
    if (visual.rigidReady[variant]) return;
    auto start=std::chrono::steady_clock::now();
    auto& meshes=visual.rigidMeshes[variant];
    meshes.clear(); meshes.reserve(visual.rigid.groups.size());
    size_t vertices=0;
    for (auto const& group:visual.rigid.groups) {
        if (debug::meshDiagnostics) logger().info("[gun-packed-group] bone={} name={} sourceVertices={}",group.bone,
            visual.model.bones[static_cast<size_t>(group.bone)%visual.model.bones.size()].name,group.vertices.size());
        auto gpu=std::make_unique<GunVisual::GroupMesh>(); // stable address, never moved/re-emplaced after drawing
        gpu->origin=model::localMeshOrigin(group.vertices);
        auto local=group.vertices;
        if (gpu->origin!=glm::vec3(0)) for (auto& v:local) v.pos-=gpu->origin;
        gpu->mesh.emplace(uploadMesh(screen,local,twoSided,gpu->count));
        vertices+=gpu->count;
        meshes.push_back(std::move(gpu));
    }
    visual.rigidReady[variant]=true;
    logger().info("[gun-rigid-build] world={} serial={} groups={} vertices={} twoSided={} CPU={:.3f} ms staticLocalHash={:016x} (one upload per group/variant/resource scope)",
        resourceKey->world,resourceSerial,meshes.size(),vertices,twoSided,
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),visual.localHash);
}

GunVisual* visualFor(IClientInstance& client, pack::ResourceId const& gunId) {
    auto const& packs = TaczMod::getInstance().gunPacks();
    auto const* gun   = packs.gun(gunId);
    if (!gun || !gun->index.display) return nullptr;
    auto const& displayId = *gun->index.display;
    auto const* display = packs.gunDisplay(displayId);
    if (!display || !display->model || !display->texture) return nullptr;

    auto [it, inserted] = cache.try_emplace(VisualKey{displayId,*display->model,*display->texture});
    GunVisual& visual   = it->second;
    if (!inserted) { ++counters.cacheHits; return visual.failed ? nullptr : &visual; }
    ++counters.cacheMisses;

    try {
        auto modelText = packs.readFile(pack::assetPath(*display->model, "geo_models", ".json"),pack::ResourceLookup::Direct);
        if (!modelText) throw std::runtime_error("model file " + display->model->toString() + " not found");
        auto png = packs.readFile(pack::assetPath(*display->texture, "textures", ".png"),pack::ResourceLookup::Direct);
        if (!png) throw std::runtime_error("texture file " + display->texture->toString() + " not found");

        visual.model     = model::parseGeoModel(pack::parseJsonText(*modelText));
        visual.enableTransparency=display->enableTransparency;
        auto const& geo  = visual.model;
        visual.armPaths = {geo.path(geo.find("lefthand_pos")), geo.path(geo.find("righthand_pos"))};
        visual.transform = model::firstPersonIdleTransform(geo);
        visual.idleView = model::positioningInverse(geo, geo.path(geo.find("idle_view")));
        int iron = geo.find("iron_view");
        visual.ironView = iron >= 0 ? model::positioningInverse(geo, geo.path(iron)) : visual.idleView;
        if (iron < 0) logger().warn("[ads] {} has no iron_view; retaining idle placement", displayId.toString());
        visual.rigid=model::compileRigidGunModel(geo);
        visual.localHash=14695981039346656037ull;
        for (auto const& group:visual.rigid.groups) {
            visual.localHash^=model::positionHash(group.vertices); visual.localHash*=1099511628211ull;
        }
        logger().info("Gun model {}: {} bones, {} rigid groups; model={} texture={}",displayId.toString(),geo.bones.size(),
            visual.rigid.groups.size(),display->model->toString(),display->texture->toString());
        if (!uploadTexture(client, *png, display->texture->ns + "/" + display->texture->path, visual)) {
            throw std::runtime_error("texture upload failed");
        }
        return &visual;
    } catch (std::exception const& e) {
        logger().error("Cannot render gun display {}: {}", displayId.toString(), e.what());
    }
    visual.failed = true;
    return nullptr;
}

MovingBlockActorRenderer* movingBlockRenderer(IClientInstance& client) {
    auto& dispatcher = client.getBlockEntityRenderDispatcher();
    return static_cast<MovingBlockActorRenderer*>(dispatcher.mRenderers.get()[BlockActorRendererId::MovingBlock].get());
}

mce::MaterialPtr const* gunMaterial(int index, ItemInHandRenderer& renderer, IClientInstance& client) {
    if (index < 0 || index >= MaterialCount) return nullptr;
    auto& slot = materials[index];
    if (!slot) {
        switch (index) {
        case 0:
            // TACZ draws guns with entityCutout (alpha test).
            slot.emplace(mce::RenderMaterialGroup::common(), HashedString{"entity_alphatest"});
            break;
        case 1:
            slot.emplace(renderer.mEntityMaterial.get());
            break;
        case 2:
            slot.emplace(renderer.mStaticMaterial.get());
            break;
        case 3:
            slot.emplace(renderer.mMatAlphaBlock.get());
            break;
        case 4:
            slot.emplace(mce::RenderMaterialGroup::common(), HashedString{"debug"});
            break;
        case 5:
            slot.emplace(mce::RenderMaterialGroup::switchable(), HashedString{"holo_hand_pointer"});
            break;
        case 6:
        case 7:
            if (auto* moving = movingBlockRenderer(client)) {
                auto layer=index==7 ? BlockRenderLayer::RenderlayerBlend : BlockRenderLayer::RenderlayerAlphatest;
                slot.emplace(moving->mBlockMaterials[static_cast<int>(layer)].get());
            } else {
                // The default constructor is not exported; an unknown name gives an empty material.
                slot.emplace(mce::RenderMaterialGroup::common(), HashedString{"tacz_missing_material"});
            }
            break;
        }
    }
    return slot->mRenderMaterialInfoPtr ? &*slot : nullptr;
}

std::string str(glm::vec3 v) { return fmt::format("({:.3f}, {:.3f}, {:.3f})", v.x, v.y, v.z); }

std::string str(glm::mat4 const& m) {
    std::string out;
    for (int row = 0; row < 4; ++row) {
        out += fmt::format("[{:.3f} {:.3f} {:.3f} {:.3f}]", m[0][row], m[1][row], m[2][row], m[3][row]);
    }
    return out;
}

// Pops a pushed matrix (the MatrixStackRef destructor is not relied on; see Lamium CellMesh).
void pop(MatrixStack::MatrixStackRef& ref) {
    ref.stack->_isDirty = true;
    if (ref.stack->sortOrigin->has_value() && (ref.stack->stack->size() - 1) <= ref.stack->sortOrigin->value()) {
        ref.stack->sortOrigin->reset();
    }
    ref.stack->stack->pop_back();
    ref.mat   = nullptr;
    ref.stack = nullptr;
}

bool drawGun(ItemInHandRenderer& renderer, BaseActorRenderContext& context) {
    auto callStart=std::chrono::steady_clock::now();
    auto session = shooter::worldToken();
    if (!session) return false; // menu meshes pass through without any world-service access
    auto pose = anim::snapshot();
    if (!model::canUseGunRenderResources(session, pose.world, pose.gun.has_value())) return false;
    auto& client = context.mClientInstance;
    auto* player = client.getLocalPlayer();
    if (!player) return false;
    auto gunId = pose.gun;

    auto& screen = context.mScreenContext;
    auto textures = client.getTextureGroup();
    auto buffers = screen.tessellator.mBufferResourceService.lock();
    auto* frameBuilder = renderDragonFrameBuilder();
    int lighting = frameBuilder && frameBuilder->isRayTracingEnabled() ? 1
        : frameBuilder && frameBuilder->isDeferredEnabled() ? 2 : 0;
    model::RenderResourceKey key{pose.world, reinterpret_cast<uintptr_t>(&client),
        reinterpret_cast<uintptr_t>(textures.get()), reinterpret_cast<uintptr_t>(buffers.get()),
        reinterpret_cast<uintptr_t>(frameBuilder), lighting};
    std::lock_guard lock(cacheMutex); // also keeps returned GunVisual alive for the whole draw
    if (!key.ready()) {
        // Live world, but a graphics/pipeline transition can temporarily withdraw services.
        // Do not create a failed cache entry or submit through absent native services.
        if (!waitingForResources) logger().info("[gun-render] world={} resource services not ready; GPU work deferred", session);
        waitingForResources = true;
        return pose.puttingAway;
    }
    waitingForResources = false;
    int requestedSampler=debug::gunSampler.load();
    if (!resourceKey || *resourceKey != key || boundSampler!=requestedSampler) {
        releaseResources("world/pipeline/resource-owner/sampling change");
        resourceKey = key;
        boundSampler=requestedSampler;
        ++resourceSerial;
        logger().info("[gun-gpu] bind world={} serial={} rtx={} deferred={}", key.world, resourceSerial,
            lighting == 1, lighting == 2);
    }
    int path=debug::gunRenderPath.load();
    if (counters.path!=path) { counters={}; counters.path=path; }
    auto buildsBefore=counters.builds,missesBefore=counters.cacheMisses;
    auto prepareStart=std::chrono::steady_clock::now();
    ++counters.hooks;
    if (counters.lastPose != pose.revision) ++counters.poseChanges;
    counters.lastPose = pose.revision;
    counters.animationFrame = pose.frame;
    auto* visual = visualFor(client, *gunId);
    if (!visual) {
        ++counters.invalidMeshes;
        return pose.puttingAway;
    }
    if (path==1 && !model::rigidGunPoseCompatible(pose.pose)) {
        if (!visual->warnedIncompatible) logger().warn("[gun-render] empty-name pose channel cannot be statically folded; using path 0 for this pose");
        visual->warnedIncompatible=true; path=0; counters.path=path;
    }
    auto visit=[&](model::Bone const& bone,model::Bone const* parent) { return model::gunVisibility(bone,parent,pose.renderState); };
    if (visual->frame.world.empty() || visual->frameRevision!=pose.revision) {
        model::evaluateBoneFrame(visual->model,visual->rigid,pose.pose,visit,visual->frame);
        visual->frameRevision=pose.revision;
    }
    bool twoSided = debug::gunTwoSided;
    if (path==0 && (!visual->mesh || pose.revision != visual->animationRevision || twoSided != visual->meshTwoSided)) {
        auto start = std::chrono::steady_clock::now();
        visual->vertices = model::bake(visual->model,visit,pose.pose,true);
        buildMesh(screen, *visual, twoSided);
        visual->animationRevision = pose.revision;
        counters.lastMesh = pose.revision;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        // Log once per visual after 120 changing poses: CPU bake + tessellation/upload submission,
        // not asynchronous GPU execution. Unchanged/static poses reuse the buffered mesh.
        if (visual->measuredFrames < 120) {
            visual->measuredMs += ms;
            visual->maxMs = std::max(visual->maxMs, ms);
            if (++visual->measuredFrames == 120) logger().info(
                "[anim] {} mesh rebuild CPU: mean {:.3f} ms, max {:.3f} ms (120 poses, {} vertices)",
                gunId->toString(), visual->measuredMs / 120, visual->maxMs, visual->vertices.size()
            );
        }
    }
    if (path==1) ensureRigidMeshes(screen,*visual,twoSided);
    auto valid=path==0 ? visual->mesh && visual->mesh->isValid()
        : std::all_of(visual->rigidMeshes[twoSided ? 1 : 0].begin(),visual->rigidMeshes[twoSided ? 1 : 0].end(),
            [](auto const& group) { return group->mesh && group->mesh->isValid(); });
    if (!valid) {
        ++counters.invalidMeshes;
        if (!visual->loggedInvalidMesh) logger().warn("[gun-render] path={} world={} cached native mesh invalid; no stale draw submitted",path,session);
        visual->loggedInvalidMesh=true;
        return pose.puttingAway;
    }
    visual->loggedInvalidMesh=false;
    counters.lastMesh=visual->frameRevision;
    auto prepareMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-prepareStart).count();

    int         materialIndex = debug::gunMaterial;
    bool alphaCompatibility=debug::gunBlend;
    bool blended=materialIndex==7 || (alphaCompatibility && visual->enableTransparency && materialIndex==6);
    if (blended) materialIndex=7;
    auto const* mat           = gunMaterial(materialIndex, renderer, client);
    if (!mat && blended && debug::gunMaterial==6) {
        if (!visual->warnedBlendFallback) logger().warn("[gun-alpha] rtx={} native moving-block blend unavailable; explicit cutout fallback (NOT translucent equivalence)",lighting==1);
        visual->warnedBlendFallback=true; materialIndex=6; blended=false; mat=gunMaterial(6,renderer,client);
    }
    auto&       camera        = screen.camera;

    int revision = debug::renderRevision;
    bool debugChanged = revision != loggedRevision;
    if (debugChanged && debug::meshDiagnostics && path==1) {
        auto const& meshes=visual->rigidMeshes[twoSided ? 1 : 0];
        for (size_t i=0;i<meshes.size();++i) {
            auto const& g=visual->rigid.groups[i];
            inspectCachedMesh(*meshes[i]->mesh,g.bone,visual->model.bones[static_cast<size_t>(g.bone)%visual->model.bones.size()].name,meshes[i]->origin);
        }
    }
    if (debugChanged) {
        loggedRevision = revision;
        logger().info(
            "Gun render: material {} ({}, {}), two-sided {}, first-person flag {}",
            materialIndex,
            materialName(materialIndex),
            mat ? "valid" : "MISSING",
            twoSided,
            static_cast<bool>(screen.isDrawingFirstPersonObjects)
        );
        logger().info(
            "Camera pos {} fwd {} right {} up {} fov {:.2f}",
            str(camera.mPosition),
            str(camera.mForward),
            str(camera.mRight),
            str(camera.mUp),
            static_cast<float>(camera.mFov)
        );
        logger().info("View {}", str(camera.viewMatrixStack->stack->back()._m.get()));
        logger().info("World {}", str(camera.worldMatrixStack->stack->back()._m.get()));
        logger().info("Proj {}", str(camera.projectionMatrixStack->stack->back()._m.get()));
        logger().info("Gun transform {}", str(visual->transform));
        logger().info("[gun-alpha-state] path={} rtx={} compatibility={} displayTransparency={} material={} blended={} opaqueGroups={} alphaGroups={} nativePipelineVariant=true (not a GPU validation)",
            path,lighting==1,alphaCompatibility,visual->enableTransparency,materialIndex,blended,visual->opaqueGroups.size(),visual->alphaGroups.size());
        if (auto* pointer=std::get_if<mce::TexturePtr>(&visual->texture)) {
            std::shared_ptr<BedrockTextureData const> data=pointer->mClientTexture;
            logger().info("[gun-sampling] mode={} rtx={} texture={}x{} metadataMips={} UV normalization={}x{} (sampler filter/wrap opaque in SDK; metadata is not a GPU capture)",
                boundSampler,lighting==1,data ? data->mTextureDescription->mWidth : 0,data ? data->mTextureDescription->mHeight : 0,
                data ? data->mTextureDescription->mMipCount : 0,visual->model.textureWidth,visual->model.textureHeight);
        }
        auto const* info = mat ? mat->mRenderMaterialInfoPtr.get() : nullptr;
        auto const* material = info ? info->mPtr.get() : nullptr;
        if (material) logger().info("[gun-depth] material={} test={} func={} writeMask={} bias={} slopeBias={} cameraNear={} cameraFar={} (legacy descriptor; GPU pipeline may override)",
            material->mFullMaterialName.get(),material->depthStencilStateDescription->depthTestEnabled,
            static_cast<int>(material->depthStencilStateDescription->depthFunc),
            static_cast<int>(material->depthStencilStateDescription->depthWriteMask),
            material->mDepthBias,material->mSlopeScaledDepthBias,camera.mZNear,camera.mZFar);
        if (material) logger().info("[gun-blend-descriptor] enable={} src={} dst={} alphaSrc={} alphaDst={} depthWriteMask={} (read-only; RenderDragon pipeline may override)",
            material->blendStateDescription->enableBlend,static_cast<int>(material->blendStateDescription->blendSource),
            static_cast<int>(material->blendStateDescription->blendDestination),static_cast<int>(material->blendStateDescription->alphaSource),
            static_cast<int>(material->blendStateDescription->alphaDestination),static_cast<int>(material->depthStencilStateDescription->depthWriteMask));
    }
    if (!mat) return true; // keep vanilla hidden so a missing material is obvious in game

    // Lighting: block materials need light-texture constants (Lamium's ghost blocks), entity
    // materials the actor ones.
    if (materialIndex == 6 || materialIndex==7) {
        auto* lightTexture = client.getLightTexture();
        if (!lightTexture) return true;
        BrightnessPair full;
        full.sky->mValue   = 15;
        full.block->mValue = 15;
        ActorShaderManager::setupShaderParameters(
            screen,
            player->getDimensionBlockSource(),
            full,
            glm::vec4{1, 1, 1, 1},
            1.f,
            true,
            *lightTexture,
            Vec2{1, 1},
            Vec4{0, 0, 1, 1}
        );
    } else {
        renderer.setupShaderParameters(screen, context, *player, context.mFrameAlpha);
    }

    // During the first-person pass the world matrix on top of the stack is the inverse view
    // rotation (logged in 0.2.2: it equals the camera basis), so multiplying onto it places the
    // gun in view space. push() copies the top. Bind the ref directly: a temporary
    // MatrixStackRef would pop in its destructor, and the manual pop below would then underflow.
    auto aiming = aim::snapshot();
    bool sameAimGun = !pose.puttingAway && aiming.world == session && aiming.gun == gunId;
    auto progress = sameAimGun ? aiming.progress : 0.f;
    auto viewProgress = sameAimGun ? aiming.viewProgress : 0.f;
    auto placement = model::firstPersonAimTransform(visual->idleView, visual->ironView, viewProgress);
    placement = model::applyConstraint(placement,model::constraintTransform(visual->model,pose.pose),viewProgress);
    glm::quat cameraRotation{1,0,0,0};
    if (auto it = pose.pose.find("camera"); cameraBonesEnabled(client) && it != pose.pose.end() && it->second.rotation)
        cameraRotation = model::scaledCameraRotation(model::cameraBoneRotation(*it->second.rotation),progress,sameAimGun ? aiming.ironZoom : 1.f);
    auto nativeProjection = camera.projectionMatrixStack->top()._m.get();
    auto baseModelFov = model::projectionFov(nativeProjection);
    float fov = baseModelFov;
    if (baseModelFov > 0) {
        auto now = std::chrono::steady_clock::now();
        if (!visual->modelFovAt || now - *visual->modelFovAt > std::chrono::seconds(1)) visual->modelFov.reset(baseModelFov);
        double dt = visual->modelFovAt ? std::chrono::duration<double>(now - *visual->modelFovAt).count() : 0;
        auto desired = std::lerp(baseModelFov, sameAimGun ? aiming.zoomModelFov : baseModelFov, progress);
        fov = visual->modelFov.update(desired, dt);
        visual->modelFovAt = now;
    }
    // RenderDragon may already have captured the projection for this pass. A temporary
    // Camera stack edit need not reach renderMesh. P_native * correction == P_model;
    // place the correction in the consumed per-draw transform (also honored by RTX).
    auto transform = model::modelProjectionTransform(nativeProjection,fov,debug::gunDepthMode)
        * glm::mat4_cast(cameraRotation) * placement;
    if (!debug::freezePose || !visual->frozen) {
        visual->transform = transform;
        visual->capturedDepthMode = debug::gunDepthMode;
    }
    visual->frozen = debug::freezePose;
    if (debug::meshDiagnostics && path==0) {
        auto& d=visual->diagnostics;
        auto clip=nativeProjection*camera.viewMatrixStack->top()._m.get()*camera.worldMatrixStack->top()._m.get()*visual->transform;
        auto now=std::chrono::steady_clock::now();
        if (d.revision!=revision) { d.previous.clear(); d.hasClip=false; d.frames=d.changes=d.topologyChanges=0; d.maxDelta=d.maxClipDelta=0; d.revision=revision; d.at=now; }
        d.previousHash=d.hash; d.hash=model::positionHash(visual->vertices);
        if (!d.previous.empty()) {
            auto delta=model::positionDelta(d.previous,visual->vertices);
            if (delta<0) ++d.topologyChanges; else d.maxDelta=std::max(d.maxDelta,delta);
            d.changes+=d.hash!=d.previousHash;
        }
        if (d.hasClip) d.maxClipDelta=std::max(d.maxClipDelta,model::matrixDelta(clip,d.previousClip));
        d.previous=visual->vertices; d.previousClip=clip; d.hasClip=true; ++d.frames;
        if (now-d.at>=std::chrono::seconds(1)) {
            // Inspect the current documented byte vector, without forcing HAL
            // generation or interpreting the SDK's empty VertexLayout.
            auto const& raw=visual->mesh->mRawData.get();
            d.rawBytes=raw.size(); d.rawHash=14695981039346656037ull;
            for (auto byte:raw) { d.rawHash^=byte; d.rawHash*=1099511628211ull; }
            auto repeat=model::bake(visual->model,visit,pose.pose,true);
            float magnitude=0;
            for (auto const& v:visual->vertices) for (int axis=0;axis<3;++axis) magnitude=std::max(magnitude,std::abs(v.pos[axis]));
            logger().info("[mesh-jitter] path=0 world={} freeze={} frames={} changes={} topologyChanges={} hash={:016x} previousHash={:016x} maxAbsPos={:.9g} consecutiveMaxDelta={:.9g} repeatMaxDelta={:.9g} tessellatorMaxDelta={:.9g} clipMatrixMaxDelta={:.9g} rawBytes={} rawHash={:016x} viewTranslation={} worldTranslation={} nativePacking=unknown frame=model-blocks cpuPosition=float32",
                session,debug::freezePose.load(),d.frames,d.changes,d.topologyChanges,d.hash,d.previousHash,magnitude,d.maxDelta,
                model::positionDelta(visual->vertices,repeat),d.uploadDelta,d.maxClipDelta,d.rawBytes,d.rawHash,
                str(glm::vec3(camera.viewMatrixStack->top()._m.get()[3])),str(glm::vec3(camera.worldMatrixStack->top()._m.get()[3])));
            d.at=now; d.frames=d.changes=d.topologyChanges=0; d.maxDelta=d.maxClipDelta=0;
        }
    }
    if (debug::meshDiagnostics && path==1) {
        auto& d=visual->diagnostics;
        auto now=std::chrono::steady_clock::now();
        if (d.revision!=revision) { d.previousBones.clear(); d.maxDelta=0; d.revision=revision; d.at=now; }
        if (d.previousBones.size()==visual->frame.world.size()) for (size_t i=0;i<d.previousBones.size();++i)
            d.maxDelta=std::max(d.maxDelta,model::matrixDelta(d.previousBones[i],visual->frame.world[i]));
        d.previousBones=visual->frame.world;
        if (now-d.at>=std::chrono::seconds(1)) {
            logger().info("[mesh-jitter] path=1 world={} freeze={} immutableLocalHash={:016x} boneMatrixMaxDelta={:.9g} poseRev={} groups={} noAnimatedVertexUpload=true nativePacking=unknown",
                session,debug::freezePose.load(),visual->localHash,d.maxDelta,pose.revision,visual->rigid.groups.size());
            d.at=now; d.maxDelta=0;
        }
    }
    if (progress < .9f) visual->loggedAimProjection = false;
    if ((progress >= .999f && !visual->loggedAimProjection) || debugChanged) {
        auto mv = camera.viewMatrixStack->top()._m.get()*camera.worldMatrixStack->top()._m.get()*visual->transform;
        auto ndc = model::sightProjection(visual->model,pose.pose,mv,nativeProjection);
        glm::mat4 sight(1);
        for (int i : visual->model.path(visual->model.find("iron_view"))) {
            auto it = pose.pose.find(visual->model.bones[i].name);
            sight *= model::partTransform(visual->model.bones[i],it == pose.pose.end() ? nullptr : &it->second);
        }
        auto clip = nativeProjection*mv*sight*glm::vec4(0,0,0,1);
        logger().info("[ads-projection] world={} nativeFov={:.3f} targetFov={:.3f} modelFov={:.3f} viewProgress={:.4f} ironOriginClip=({:.6f},{:.6f},{:.6f},{:.6f}) sightAxisNDC=({:.6f},{:.6f}) depthMode={} capturedDepthMode={} freeze={} nativeP22={:.9g} nativeP32={:.9g} projectionViaModel=true",
            session,baseModelFov,sameAimGun ? aiming.zoomModelFov : baseModelFov,fov,viewProgress,clip.x,clip.y,clip.z,clip.w,ndc.x,ndc.y,debug::gunDepthMode.load(),visual->capturedDepthMode,debug::freezePose.load(),nativeProjection[2][2],nativeProjection[3][2]);
        visual->loggedAimProjection = true;
    }
    auto world            = camera.worldMatrixStack->push(false);
    world.stack->_isDirty = true;
    world.mat->_m         = world.mat->_m.get() * visual->transform;

    TextureVariant const& texture = (materialIndex == 4 || materialIndex == 5) ? TextureVariant{}
        : alphaCompatibility && !blended ? visual->cutoutTexture : visual->texture;
    auto drawStart=std::chrono::steady_clock::now();
    size_t gunCalls=0,armCalls=0;
    try {
        if (path==0) {
            visual->mesh->renderMesh(screen, *mat, texture, 0, visual->vertexCount, OffscreenCaptureDescription{}, nullptr);
            ++gunCalls; ++counters.draws;
        } else {
            auto const& meshes=visual->rigidMeshes[twoSided ? 1 : 0];
            auto drawGroup=[&](size_t i) {
                auto const& group=visual->rigid.groups[i];
                if (!model::drawableRigidBone(visual->frame,group.bone)) return;
                auto boneMatrix=camera.worldMatrixStack->push(false);
                boneMatrix.stack->_isDirty=true;
                boneMatrix.mat->_m=boneMatrix.mat->_m.get()*model::localMeshMatrix(visual->frame.world[group.bone],meshes[i]->origin);
                try {
                    meshes[i]->mesh->renderMesh(screen,*mat,texture,0,meshes[i]->count,OffscreenCaptureDescription{},nullptr);
                    ++gunCalls; ++counters.draws;
                } catch (...) { pop(boneMatrix); throw; }
                pop(boneMatrix);
            };
            if (blended) {
                // Stable original DFS order within each phase. Every group still
                // uses the same native blend material, as Java's display flag does.
                for (auto i:visual->opaqueGroups) drawGroup(i);
                for (auto i:visual->alphaGroups) drawGroup(i);
            } else for (size_t i=0;i<visual->rigid.groups.size();++i) drawGroup(i);
        }
        // TACZ delegates skin draws until AFTER the gun's draw, on the same captured pose
        // stack. Arms keep their own texture/material and append only their bone matrices.
        armCalls=drawPlayerArms(screen, client, *player, visual->model, visual->armPaths, visual->frame,
            session, resourceSerial, lighting == 1);
    } catch (...) {
        pop(world);
        throw;
    }
    pop(world);
    auto now=std::chrono::steady_clock::now();
    auto drawMs=std::chrono::duration<double,std::milli>(now-drawStart).count();
    auto totalMs=std::chrono::duration<double,std::milli>(now-callStart).count();
    ++counters.frames; counters.armDraws+=armCalls;
    counters.totalMs+=totalMs; counters.maxMs=std::max(counters.maxMs,totalMs);
    counters.prepareMs+=prepareMs; counters.drawMs+=drawMs;
    bool steady=counters.cacheMisses==missesBefore && (path==0 || counters.builds==buildsBefore);
    if (steady && !debug::meshDiagnostics && counters.warmFrames<120) {
        counters.warmMs+=totalMs; counters.warmMax=std::max(counters.warmMax,totalMs);
        counters.warmPrepare+=prepareMs; counters.warmDraw+=drawMs;
        if (++counters.warmFrames==120) logger().info(
            "[gun-perf] path={} rtx={} N=120 steady CPU mean={:.3f} max={:.3f} prepareMean={:.3f} gunAndArmDrawMean={:.3f} ms lastGunCalls={} lastArmCalls={} GPU-not-timed",
            path,lighting==1,counters.warmMs/120,counters.warmMax,counters.warmPrepare/120,counters.warmDraw/120,gunCalls,armCalls);
    }
    auto elapsed=std::chrono::duration<double>(now-counters.start).count();
    if (elapsed>=1) {
        logger().info("[gun-render] path={} rtx={} deferred={} seconds={:.2f} frames={} hooks={} builds={} gunDrawCalls={} armDrawCalls={} gunCallsPerFrame={:.2f} lastGunCalls={} cacheHits={} cacheMisses={} CPUmean={:.3f} CPUmax={:.3f} prepareMean={:.3f} gunAndArmDrawMean={:.3f} ms poseChanges={} animFrame={} poseRev={} matrixRev={} invalidMeshes={} groups={} firstPerson={} meshDiagnostics={} trace=call-site-only GPU-not-timed",
            path,lighting==1,lighting==2,elapsed,counters.frames,counters.hooks,counters.builds,counters.draws,counters.armDraws,
            double(counters.draws)/counters.frames,gunCalls,counters.cacheHits,counters.cacheMisses,counters.totalMs/counters.frames,counters.maxMs,
            counters.prepareMs/counters.frames,counters.drawMs/counters.frames,counters.poseChanges,counters.animationFrame,counters.lastPose,
            counters.lastMesh,counters.invalidMeshes,visual->rigid.groups.size(),static_cast<bool>(screen.isDrawingFirstPersonObjects),debug::meshDiagnostics.load());
        counters.start=now;
        counters.hooks=counters.builds=counters.draws=counters.poseChanges=counters.invalidMeshes=0;
        counters.cacheHits=counters.cacheMisses=counters.frames=counters.armDraws=0;
        counters.totalMs=counters.maxMs=counters.prepareMs=counters.drawMs=0;
    }
    return true;
}

// Deliberately no global Mesh::_renderMesh hook: LL's by-value TextureList is an opaque
// empty static_vector placeholder, not an ABI-safe native container. Call-site counters
// above observe only our fully typed renderMesh(..., TextureVariant const&, ...) call.
LL_TYPE_INSTANCE_HOOK(
    FirstPersonGunHook,
    ll::memory::HookPriority::Normal,
    ItemInHandRenderer,
    &ItemInHandRenderer::renderFirstPerson,
    void,
    BaseActorRenderContext& context,
    Matrix const&           previousProjection,
    ItemContextFlags        flags
) {
    try {
        if (drawGun(*this, context)) return; // replaces the vanilla hand and held item, as TACZ does
    } catch (std::exception const& e) {
        logger().error("First-person gun render failed: {}", e.what());
    } catch (...) {
        logger().error("First-person gun render failed");
    }
    auto pose = anim::snapshot();
    // A gun draw may already have succeeded before a skin draw fails. Do not draw vanilla
    // arms/items over it. Menu/stale snapshots still pass through to the native renderer.
    if (model::canUseGunRenderResources(shooter::worldToken(), pose.world, pose.gun.has_value())) return;
    origin(context, previousProjection, flags);
}

bool hooked = false;

} // namespace

void enable() {
    enableCameraBones();
    enableSkinTextureSource(); // idempotent, including mod re-enable
    hooked = FirstPersonGunHook::hook() == 0;
    if (!hooked) logger().warn("Cannot hook first-person rendering; guns will look like their vanilla item");
}

void disable() {
    disableCameraBones();
    disableSkinTextureSource();
    if (hooked) FirstPersonGunHook::unhook();
    hooked = false;
    std::lock_guard lock(cacheMutex);
    releaseResources("mod disable");
}

void releaseWorld(IClientInstance const* client) {
    std::lock_guard lock(cacheMutex);
    if (resourceKey && resourceKey->client == reinterpret_cast<uintptr_t>(client)) releaseResources("world exit");
}

void diagnoseFrame() {
    static auto start = std::chrono::steady_clock::now();
    static uint64_t frames = 0;
    auto session = shooter::worldToken();
    if (!session) {
        start = std::chrono::steady_clock::now();
        frames = 0;
        return;
    }
    ++frames;
    auto now = std::chrono::steady_clock::now();
    auto seconds = std::chrono::duration<double>(now - start).count();
    if (seconds < 1) return;
    auto pose = anim::snapshot();
    if (model::canUseGunRenderResources(session, pose.world, pose.gun.has_value())) {
        auto* builder = renderDragonFrameBuilder();
        logger().info("[anim-frame] world={} frames={} seconds={:.2f} animFrame={} poseRev={} pipelinePresent={} rtx={} deferred={}",
            pose.world, frames, seconds, pose.frame, pose.revision,
            builder != nullptr, builder && builder->isRayTracingEnabled(), builder && builder->isDeferredEnabled());
    }
    start = now;
    frames = 0;
}

} // namespace tacz::client::render
