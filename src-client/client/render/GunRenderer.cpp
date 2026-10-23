#include "client/render/GunRenderer.h"

#include <exception>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

#include <glm/glm.hpp>
#include <stb_image.h>

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"

#include "mc/client/game/IClientInstance.h"
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
#include "mc/deps/core/string/HashedString.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/core_graphics/TextureSetLayerType.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/deps/minecraft_renderer/game/ItemContextFlags.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTexture.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
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
#include "model/GeoModel.h"
#include "model/GunModelRules.h"
#include "mod/TaczMod.h"
#include "nbt/GunNbt.h"
#include "pack/GunPackParser.h"

namespace tacz::client::render {
namespace {

using TextureVariant = std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture>;

ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }

// Everything needed to draw one gun display, built on first use on the render thread.
struct GunVisual {
    bool                              failed = false; // logged once, then the gun falls back to vanilla
    glm::mat4                         transform{1.0f};
    std::vector<model::Vertex>        vertices; // baked rest pose, kept to rebuild the mesh
    model::GeoModel                   model;
    uint64_t                          animationRevision = 0;
    bool                              animated = false;
    size_t                            measuredFrames = 0;
    double                            measuredMs = 0;
    double                            maxMs = 0;
    std::optional<mce::Mesh>          mesh;
    bool                              meshTwoSided = false;
    uint                              vertexCount  = 0;
    std::unique_ptr<ResourceLocation> textureLocation;
    TextureVariant                    texture;
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

std::optional<pack::ResourceId> heldGunId(Player const& player) {
    auto const& tag = player.getSelectedItem().mUserData;
    if (!tag || !tag->contains(nbt::GunId, Tag::String)) return std::nullopt;
    return pack::ResourceId::parse(static_cast<std::string const&>((*tag)[nbt::GunId]));
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
    visual.textureLocation = std::make_unique<ResourceLocation>(Core::PathView("tacz/" + name), ResourceFileSystem::Raw);
    auto& texture          = group->uploadTexture(*visual.textureLocation, cg::ImageBuffer(std::move(image)));
    visual.texture         = mce::TexturePtr(texture, *visual.textureLocation, cg::TextureSetLayerType::Color);
    logger().info("Uploaded gun texture {} ({}x{})", name, w, h);
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
}

GunVisual* visualFor(ScreenContext& screen, IClientInstance& client, pack::ResourceId const& gunId) {
    auto const& packs = TaczMod::getInstance().gunPacks();
    auto const* gun   = packs.gun(gunId);
    if (!gun || !gun->index.display) return nullptr;
    auto const& displayId = *gun->index.display;

    std::lock_guard lock(cacheMutex);
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
        visual.transform = model::firstPersonIdleTransform(geo);
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
    auto& client = context.mClientInstance;
    auto* player = client.getLocalPlayer();
    if (!player) return false;
    auto gunId = heldGunId(*player);
    if (!gunId) return false;

    auto& screen = context.mScreenContext;
    auto* visual = visualFor(screen, client, *gunId);
    if (!visual || !visual->mesh || !visual->mesh->isValid()) return false;

    bool twoSided = debug::gunTwoSided;
    auto pose = anim::snapshot(*gunId);
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
    if (revision != loggedRevision) {
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
    auto world            = camera.worldMatrixStack->push(false);
    world.stack->_isDirty = true;
    world.mat->_m         = world.mat->_m.get() * visual->transform;

    TextureVariant const& texture = (materialIndex == 4 || materialIndex == 5) ? TextureVariant{} : visual->texture;
    try {
        visual->mesh->renderMesh(screen, *mat, texture, 0, visual->vertexCount, OffscreenCaptureDescription{}, nullptr);
    } catch (...) {
        pop(world);
        throw;
    }
    pop(world);
    return true;
}

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
    origin(context, previousProjection, flags);
}

bool hooked = false;

} // namespace

void enable() {
    hooked = FirstPersonGunHook::hook() == 0;
    if (!hooked) logger().warn("Cannot hook first-person rendering; guns will look like their vanilla item");
}

void disable() {
    if (hooked) FirstPersonGunHook::unhook();
    hooked = false;
    std::lock_guard lock(cacheMutex);
    cache.clear();
    for (auto& slot : materials) slot.reset();
    loggedRevision = -1;
}

} // namespace tacz::client::render
