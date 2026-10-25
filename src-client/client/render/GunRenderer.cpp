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
#include "client/render/CameraBones.h"
#include "mod/TaczMod.h"
#include "nbt/GunNbt.h"
#include "pack/GunPackParser.h"

namespace tacz::client::render {
namespace {

using TextureVariant = std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture>;

using FirstPersonSignature = void (ItemInHandRenderer::*)(BaseActorRenderContext&, Matrix const&, ItemContextFlags);
static_assert(std::is_same_v<decltype(&ItemInHandRenderer::renderFirstPerson), FirstPersonSignature>);

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

// Everything needed to draw one gun display, built on first use on the render thread.
struct GunVisual {
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
    ArmPaths                          armPaths;
    uint64_t                          animationRevision = 0;
    bool                              animated = false;
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
        std::chrono::steady_clock::time_point at=std::chrono::steady_clock::now();
    } diagnostics;
    std::unique_ptr<ResourceLocation> textureLocation;
    TextureVariant                    texture;
    std::weak_ptr<mce::TextureGroup>   textureOwner;
};

// Render diagnostics (/tacz rdebug). Materials:
//   0 entity_alphatest   1 renderer entity    2 renderer static
//   3 renderer alpha block   4 debug (vertex colour)   5 holo_hand_pointer
//   6 moving-block alphatest (what Lamium uses for textured ghost blocks)
// Verified in game (0.2.2): 4 and 6 draw; 2 is not populated on ItemInHandRenderer.
constexpr int MaterialCount = 7;

char const* materialName(int index) {
    static constexpr char const* names[MaterialCount] = {
        "entity_alphatest",
        "renderer entity",
        "renderer static",
        "renderer alpha block",
        "debug",
        "holo_hand_pointer",
        "moving-block alphatest",
    };
    return index >= 0 && index < MaterialCount ? names[index] : "?";
}

std::mutex cacheMutex;

// Meshes, textures and materials are GPU resources. They are intentionally leaked at process
// exit: a static destructor could run after the renderer is gone. disable() empties them.
auto& cache     = *new std::map<pack::ResourceId, GunVisual>;
auto& materials = *new std::array<std::optional<mce::MaterialPtr>, MaterialCount>;
int   loggedRevision = -1;
std::optional<model::RenderResourceKey> resourceKey;
uint64_t resourceSerial = 0;
struct RenderCounters {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    uint64_t hooks = 0, builds = 0, draws = 0, poseChanges = 0, invalidMeshes = 0;
    uint64_t lastPose = 0, lastMesh = 0, animationFrame = 0;
    size_t cacheType = 0, cpuPositions = 0;
    uint64 cacheId = 0;
    bool temporary = false;
} counters;
bool waitingForResources = false;

// Caller holds cacheMutex. TexturePtr is released before asking its ORIGINAL group to
// unload our Raw resource. Never query a new world's group to release an old name.
void releaseResources(char const* reason) {
    releasePlayerArms(reason);
    for (auto& [id, visual] : cache) {
        visual.mesh.reset();
        visual.texture = {};
        if (visual.textureLocation) {
            auto owner = visual.textureOwner.lock();
            bool unloaded = owner && owner->unloadTexture(*visual.textureLocation, false);
            logger().info("[gun-gpu] release {} world={} serial={} reason={} textureUnloaded={}", id.toString(),
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
    mce::Image image(static_cast<uint32>(w), static_cast<uint32>(h), mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::SRGB);
    image.mAlphaUsage = mce::AlphaUsage::Transparent;
    image.setRawImage(mce::Blob(pixels, static_cast<size_t>(w) * static_cast<size_t>(h) * 4));
    stbi_image_free(pixels);

    auto group = client.getTextureGroup();
    if (!group) return false;
    auto runtimeName = fmt::format("tacz/runtime/{}/{}/{}", resourceKey->world, resourceSerial, name);
    visual.textureLocation = std::make_unique<ResourceLocation>(Core::PathView(runtimeName), ResourceFileSystem::Raw);
    auto& texture          = group->uploadTexture(*visual.textureLocation, cg::ImageBuffer(std::move(image)));
    visual.texture         = mce::TexturePtr(texture, *visual.textureLocation, cg::TextureSetLayerType::Color);
    visual.textureOwner = group;
    logger().info("[gun-gpu] create {} ({}x{}) world={} serial={} group={}", name, w, h,
        resourceKey->world, resourceSerial, fmt::ptr(group.get()));
    return true;
}

// Baked vertices -> buffered quad mesh. With `twoSided` every quad is emitted a second time
// with reversed winding, so face culling cannot hide the model.
void buildMesh(ScreenContext& screen, GunVisual& visual, bool twoSided) {
    auto const& vertices = visual.vertices;
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
    visual.diagnostics.uploadDelta=-1; // never reuse an older mesh's measurement
    if (debug::meshDiagnostics) {
        auto const& positions=data.mPositions.get();
        auto& d=visual.diagnostics;
        size_t expected=vertices.size()*(twoSided ? 2 : 1);
        d.uploadDelta=positions.size()==expected ? 0.f : -1.f;
        if (d.uploadDelta==0) for (size_t i=0;i<positions.size();++i) {
            size_t quad=i/(twoSided ? 8 : 4), corner=i%(twoSided ? 8 : 4);
            size_t source=quad*4+(corner<4 ? corner : 7-corner);
            for (int axis=0;axis<3;++axis) d.uploadDelta=std::max(d.uploadDelta,std::abs(positions[i][axis]-vertices[source].pos[axis]));
        }
    }
    if (!visual.mesh) logger().info(
        "Gun mesh: {} vertices (two-sided {}), uv0={}, normals={}, colors={}",
        batch.mCount,
        twoSided,
        data.mTextureUVs[0]->size(),
        data.mNormals->size(),
        data.mColors->size()
    );
    visual.vertexCount  = batch.mCount;
    visual.meshTwoSided = twoSided;
    visual.mesh.emplace(batch.end(Tessellator::UploadMode::Buffered, "tacz gun", SupplementaryFieldAutoGenerationMode{}));
    ++counters.builds;
}

GunVisual* visualFor(ScreenContext& screen, IClientInstance& client, pack::ResourceId const& gunId) {
    auto const& packs = TaczMod::getInstance().gunPacks();
    auto const* gun   = packs.gun(gunId);
    if (!gun || !gun->index.display) return nullptr;
    auto const& displayId = *gun->index.display;

    auto [it, inserted] = cache.try_emplace(displayId);
    GunVisual& visual   = it->second;
    if (!inserted) return visual.failed ? nullptr : &visual;

    try {
        auto const* display = packs.gunDisplay(displayId);
        if (!display || !display->model || !display->texture) throw std::runtime_error("display has no model or texture");
        auto modelText = packs.readFile(pack::assetPath(*display->model, "geo_models", ".json"));
        if (!modelText) throw std::runtime_error("model file " + display->model->toString() + " not found");
        auto png = packs.readFile(pack::assetPath(*display->texture, "textures", ".png"));
        if (!png) throw std::runtime_error("texture file " + display->texture->toString() + " not found");

        visual.model     = model::parseGeoModel(pack::parseJsonText(*modelText));
        auto const& geo  = visual.model;
        visual.armPaths = {geo.path(geo.find("lefthand_pos")), geo.path(geo.find("righthand_pos"))};
        visual.transform = model::firstPersonIdleTransform(geo);
        visual.idleView = model::positioningInverse(geo, geo.path(geo.find("idle_view")));
        int iron = geo.find("iron_view");
        visual.ironView = iron >= 0 ? model::positioningInverse(geo, geo.path(iron)) : visual.idleView;
        if (iron < 0) logger().warn("[ads] {} has no iron_view; retaining idle placement", displayId.toString());
        model::GunRenderState state;
        visual.vertices = model::bake(geo, [&](model::Bone const& bone, model::Bone const* parent) {
            return model::gunVisibility(bone, parent, state);
        });
        logger().info("Gun model {}: {} bones, {} vertices", displayId.toString(), geo.bones.size(), visual.vertices.size());
        buildMesh(screen, visual, debug::gunTwoSided);
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
            if (auto* moving = movingBlockRenderer(client)) {
                slot.emplace(moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get());
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
    if (!resourceKey || *resourceKey != key) {
        releaseResources("world/pipeline/resource-owner change");
        resourceKey = key;
        ++resourceSerial;
        logger().info("[gun-gpu] bind world={} serial={} rtx={} deferred={}", key.world, resourceSerial,
            lighting == 1, lighting == 2);
    }
    ++counters.hooks;
    if (counters.lastPose != pose.revision) ++counters.poseChanges;
    counters.lastPose = pose.revision;
    counters.animationFrame = pose.frame;
    auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - counters.start).count();
    if (elapsed >= 1) {
        logger().info("[gun-render] rtx={} deferred={} seconds={:.2f} hooks={} builds={} draws={} poseChanges={} animFrame={} poseRev={} meshRev={} invalidMeshes={} cacheType={} cacheId={} temporary={} cpuPositions={} firstPerson={} path=renderFirstPerson->Mesh::renderMesh trace=call-site-only",
            lighting == 1, lighting == 2, elapsed, counters.hooks, counters.builds, counters.draws,
            counters.poseChanges, counters.animationFrame, counters.lastPose, counters.lastMesh, counters.invalidMeshes,
            counters.cacheType, counters.cacheId, counters.temporary, counters.cpuPositions,
            static_cast<bool>(screen.isDrawingFirstPersonObjects));
        counters.start = std::chrono::steady_clock::now();
        counters.hooks = counters.builds = counters.draws = counters.poseChanges = counters.invalidMeshes = 0;
    }
    auto* visual = visualFor(screen, client, *gunId);
    if (!visual || !visual->mesh || !visual->mesh->isValid()) {
        ++counters.invalidMeshes;
        return pose.puttingAway;
    }

    auto const& meshKey = visual->mesh->mCacheKey.get();
    counters.cacheType = meshKey.index();
    counters.cacheId = std::get_if<uint64>(&meshKey) ? std::get<uint64>(meshKey) : 0;
    counters.temporary = visual->mesh->mTemporary;
    counters.cpuPositions = visual->mesh->mMeshData->mPositions->size();

    bool twoSided = debug::gunTwoSided;
    if (pose.revision != visual->animationRevision || twoSided != visual->meshTwoSided) {
        auto start = std::chrono::steady_clock::now();
        if (!pose.pose.empty() || visual->animated) {
            model::GunRenderState state;
            visual->vertices = model::bake(visual->model, [&](model::Bone const& bone, model::Bone const* parent) {
                return model::gunVisibility(bone, parent, state);
            }, pose.pose);
        }
        buildMesh(screen, *visual, twoSided);
        visual->animationRevision = pose.revision;
        counters.lastMesh = pose.revision;
        visual->animated = !pose.pose.empty();
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

    int         materialIndex = debug::gunMaterial;
    auto const* mat           = gunMaterial(materialIndex, renderer, client);
    auto&       camera        = screen.camera;

    int revision = debug::renderRevision;
    bool debugChanged = revision != loggedRevision;
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
        auto const* info = mat ? mat->mRenderMaterialInfoPtr.get() : nullptr;
        auto const* material = info ? info->mPtr.get() : nullptr;
        if (material) logger().info("[gun-depth] material={} test={} func={} writeMask={} bias={} slopeBias={} cameraNear={} cameraFar={} (legacy descriptor; GPU pipeline may override)",
            material->mFullMaterialName.get(),material->depthStencilStateDescription->depthTestEnabled,
            static_cast<int>(material->depthStencilStateDescription->depthFunc),
            static_cast<int>(material->depthStencilStateDescription->depthWriteMask),
            material->mDepthBias,material->mSlopeScaledDepthBias,camera.mZNear,camera.mZFar);
    }
    if (!mat) return true; // keep vanilla hidden so a missing material is obvious in game

    // Lighting: block materials need light-texture constants (Lamium's ghost blocks), entity
    // materials the actor ones.
    if (materialIndex == 6) {
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
    if (auto it = pose.pose.find("camera"); aim::cameraBonesEnabled() && it != pose.pose.end() && it->second.rotation)
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
    if (debug::meshDiagnostics) {
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
            model::GunRenderState state;
            auto repeat=model::bake(visual->model,[&](auto const& bone,auto* parent){return model::gunVisibility(bone,parent,state);},pose.pose);
            float magnitude=0;
            for (auto const& v:visual->vertices) for (int axis=0;axis<3;++axis) magnitude=std::max(magnitude,std::abs(v.pos[axis]));
            logger().info("[mesh-jitter] world={} freeze={} frames={} changes={} topologyChanges={} hash={:016x} previousHash={:016x} maxAbsPos={:.9g} consecutiveMaxDelta={:.9g} repeatMaxDelta={:.9g} tessellatorMaxDelta={:.9g} clipMatrixMaxDelta={:.9g} rawBytes={} rawHash={:016x} viewTranslation={} worldTranslation={} nativePacking=unknown frame=model-blocks cpuPosition=float32",
                session,debug::freezePose.load(),d.frames,d.changes,d.topologyChanges,d.hash,d.previousHash,magnitude,d.maxDelta,
                model::positionDelta(visual->vertices,repeat),d.uploadDelta,d.maxClipDelta,d.rawBytes,d.rawHash,
                str(glm::vec3(camera.viewMatrixStack->top()._m.get()[3])),str(glm::vec3(camera.worldMatrixStack->top()._m.get()[3])));
            d.at=now; d.frames=d.changes=d.topologyChanges=0; d.maxDelta=d.maxClipDelta=0;
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

    TextureVariant const& texture = (materialIndex == 4 || materialIndex == 5) ? TextureVariant{} : visual->texture;
    try {
        visual->mesh->renderMesh(screen, *mat, texture, 0, visual->vertexCount, OffscreenCaptureDescription{}, nullptr);
        ++counters.draws;
        // TACZ delegates skin draws until AFTER the gun's draw, on the same captured pose
        // stack. Arms keep their own texture/material and append only their bone matrices.
        drawPlayerArms(screen, client, *player, visual->model, visual->armPaths, pose.pose,
            session, resourceSerial, lighting == 1);
    } catch (...) {
        pop(world);
        throw;
    }
    pop(world);
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
