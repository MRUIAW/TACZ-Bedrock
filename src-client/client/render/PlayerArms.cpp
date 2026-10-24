#include "client/render/PlayerArms.h"
#include "client/render/SkinTextureSource.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <variant>

#include <Windows.h>
#include <stb_image.h>
#include <fmt/format.h>

#include "ll/api/io/Logger.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "mc/client/renderer/ActorShaderManager.h"
#include "mc/client/renderer/SupplementaryFieldAutoGenerationMode.h"
#include "mc/client/renderer/Tessellator.h"
#include "mc/client/renderer/TextureGroup.h"
#include "mc/client/renderer/blockactor/BlockActorRenderDispatcher.h"
#include "mc/client/renderer/blockactor/MovingBlockActorRenderer.h"
#include "mc/client/renderer/ptexture/LightTexture.h"
#include "mc/deps/core/file/PathView.h"
#include "mc/deps/core/image/Image.h"
#include "mc/deps/core/math/Matrix.h"
#include "mc/deps/core/math/Vec2.h"
#include "mc/deps/core/math/Vec4.h"
#include "mc/deps/core/resource/ResourceFileSystem.h"
#include "mc/deps/core/resource/ResourceLocation.h"
#include "mc/deps/core_graphics/ImageBuffer.h"
#include "mc/deps/core_graphics/TextureSetLayerType.h"
#include "mc/deps/core_graphics/enums/PrimitiveMode.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTexture.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/level/block/BlockRenderLayer.h"
#include "mc/world/level/block/BrightnessPair.h"
#include "mc/world/level/block/actor/BlockActorRendererId.h"

#include "model/PlayerArms.h"
#include "mod/TaczMod.h"

namespace tacz::client::render {
namespace {
using Clock = std::chrono::steady_clock;
using Texture = std::variant<std::monostate, mce::TexturePtr, mce::ClientTexture, mce::ServerTexture>;
ll::io::Logger& logger() { return TaczMod::getInstance().getSelf().getLogger(); }
double msSince(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

struct SkinPixels {
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    bool slim = false, fallback = false;
    char const* reason = "classic";
};

// Read the installed game's own default texture; no Mojang image is redistributed and no
// working-directory/launcher-profile path is assumed. Only used on skin selection changes
// or the 1 Hz skin probe, never on every animation pose.
bool readDefault(SkinPixels& out) {
    std::wstring exe(32768, L'\0');
    auto length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length >= exe.size()) return false;
    exe.resize(length);
    auto path = std::filesystem::path(exe).parent_path() / "data" / "skin_packs" / "vanilla"
        / (out.slim ? "alex.png" : "steve.png");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    auto size = file.tellg();
    if (size <= 0 || size > 4 * 1024 * 1024) return false;
    std::vector<unsigned char> png(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(png.data()), static_cast<std::streamsize>(png.size()))) return false;
    int w = 0, h = 0, channels = 0;
    auto* decoded = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 4);
    if (!decoded) return false;
    if ((w != 64 && w != 128) || h != w) { stbi_image_free(decoded); return false; }
    out.width = static_cast<uint32_t>(w);
    out.height = static_cast<uint32_t>(h);
    out.rgba.assign(decoded, decoded + size_t(w) * h * 4);
    stbi_image_free(decoded);
    out.fallback = true;
    return true;
}

struct ArmCache {
    std::array<std::optional<mce::Mesh>, 4> meshes; // left base/sleeve, right base/sleeve
    std::array<std::optional<mce::MaterialPtr>, 2> materials; // opaque base / blended sleeve
    Texture texture;
    std::unique_ptr<ResourceLocation> location;
    std::weak_ptr<mce::TextureGroup> owner;
    std::weak_ptr<BedrockTextureData const> borrowedData;
    uintptr_t borrowedBlock = 0;
    uint64_t borrowedRevision = 0;
    bool textureReady = false;
    std::optional<bool> meshSlim;
    std::weak_ptr<void> loggedSkinOwner;
    std::string skinLogKey;
    bool skinDrawLogged = false;
    uint64_t hash = 0;
    bool slim = false, fallback = false;
    uint32_t width = 0;
    Clock::time_point nextProbe{};
    bool missingLogged = false;
    bool materialLogged = false;
    size_t frames = 0, builds = 0, draws = 0;
    double totalMs = 0, probeMs = 0, buildMs = 0, drawMs = 0, maxMs = 0;
};
// Same deliberate process-exit leak as GunRenderer; release is explicit BEFORE teardown.
auto& cache = *new std::unique_ptr<ArmCache>;
uint64_t skinSerial = 0;

void dropTexture(ArmCache& a) {
    a.texture = {};
    a.textureReady = false;
    a.borrowedData.reset();
    a.borrowedBlock = 0;
    a.borrowedRevision = 0;
    if (a.location) {
        if (auto owner = a.owner.lock()) owner->unloadTexture(*a.location, false);
        a.location.reset();
    }
    a.owner.reset();
}

void ensureArmMeshes(ScreenContext& screen, ArmCache& a) {
    bool valid = std::all_of(a.meshes.begin(), a.meshes.end(), [](auto const& mesh) { return mesh && mesh->isValid(); });
    if (a.meshSlim == a.slim && valid) return;
    auto start = Clock::now();
    for (int side = 0; side < 2; ++side) for (int layer = 0; layer < 2; ++layer) {
        auto vertices = model::playerArmVertices(side == 0 ? model::Arm::Left : model::Arm::Right, a.slim, layer != 0);
        Tessellator batch(screen.tessellator.mBufferResourceService);
        batch.begin({}, mce::PrimitiveMode::QuadList, 24, false);
        batch.color(1, 1, 1, 1);
        for (auto const& vertex : vertices) {
            batch.mNextUV[0] = vertex.uv;
            batch.mNextUV[1] = glm::vec2{1, 1};
            batch.mNextNormal = glm::vec4(vertex.normal, 0);
            batch.vertex(vertex.pos.x, vertex.pos.y, vertex.pos.z);
        }
        a.meshes[side * 2 + layer].emplace(batch.end(Tessellator::UploadMode::Buffered,
            "tacz player arm", SupplementaryFieldAutoGenerationMode{}));
        ++a.builds;
    }
    a.meshSlim = a.slim;
    a.buildMs += msSince(start);
}

void probeSkin(ScreenContext& screen, IClientInstance& client, LocalPlayer& player,
    uint64_t world, uint64_t resourceSerial) {
    auto& a = *cache;
    auto now = Clock::now();
    if (now < a.nextProbe) return;
    a.nextProbe = now + std::chrono::seconds(1);
    auto source = findRenderSkinTexture(player);
    uint32_t width = source.texture ? source.texture->mTextureDescription->mWidth : 0;
    uint32_t height = source.texture ? source.texture->mTextureDescription->mHeight : 0;
    auto layout = model::renderArmSkinLayout(width, height, source.geometry);
    char const* reason = !source.refPresent ? "skin-ref-missing" : !source.ownerPresent ? "skin-owner-missing"
        : !source.revision ? "renderer-texture-not-observed" : !source.texture ? "renderer-texture-expired"
        : source.texture->mIsMissingTexture == IsMissingTexture::Yes ? "renderer-missing-texture"
        : !(static_cast<unsigned>(source.texture->mTextureLoadState) & static_cast<unsigned>(TextureLoadState::LoadedBit)) ? "renderer-texture-pending"
        : !source.texture->mClientTexture.get() ? "renderer-texture-no-handle"
        : !layout ? "unverified-renderer-arm-layout" : "renderer-handle";
    bool borrowed = std::string_view(reason) == "renderer-handle";
    auto logKey = fmt::format("{}:{}:{}:{}:{}:{}", source.revision, reason, width, height, source.geometry, source.ownerState);
    bool sameOwner = !a.loggedSkinOwner.owner_before(source.skinOwner) && !source.skinOwner.owner_before(a.loggedSkinOwner);
    if (a.skinLogKey != logKey || !sameOwner) {
        a.skinLogKey = logKey;
        a.loggedSkinOwner = source.skinOwner;
        a.skinDrawLogged = false;
        a.missingLogged = false;
        logger().info("[arm-skin] world={} revision={} observedUpdates={} source={} reason={} ref={} owner={} ownerState={} ownerThreadHash={} renderThread={} renderThreadHash={} updateThread={} imageProvided={} input={}x{} depth={} format={} bytes={} gpu={}x{} geometry={} (CPU image not read by arms)",
            world, source.revision, source.updatesSeen, borrowed ? "renderer-handle" : "default", reason, source.refPresent,
            source.ownerPresent, source.ownerState, source.ownerThreadHash, GetCurrentThreadId(), source.renderThreadHash,
            source.updateThread, source.imageProvided, source.inputWidth, source.inputHeight, source.inputDepth, source.inputFormat,
            source.inputBytes, width, height, source.geometry);
        logger().info("[arm-skin-match] world={} revision={} match={} localOwner={:#x} inputOwner={:#x} storedOwner={:#x} info={:#x} dataAtUpdate={:#x} blockAtUpdate={:#x} dataNow={:#x} (snapshot addresses only; match uses shared control blocks)",
            world, source.revision, source.matchKind, source.localIdentity, source.inputIdentity, source.storedIdentity,
            source.infoIdentity, source.textureIdentityAtUpdate, source.handleIdentityAtUpdate,
            reinterpret_cast<uintptr_t>(source.texture.get()));
    }
    if (borrowed) {
        auto block = reinterpret_cast<uintptr_t>(source.texture->mClientTexture->mResourcePointerBlock.get());
        if (!a.textureReady || a.fallback || a.borrowedRevision != source.revision || a.borrowedBlock != block
            || a.borrowedData.lock() != source.texture) {
            dropTexture(a); // unloads only our own Raw fallback, NEVER the engine's skin
            a.texture = source.texture->mClientTexture.get(); // fully typed shared resource handle
            a.borrowedData = source.texture;
            a.borrowedBlock = block;
            a.borrowedRevision = source.revision;
            a.textureReady = true;
            a.slim = *layout;
            a.width = width;
            a.fallback = false;
            a.frames = a.builds = a.draws = 0;
            a.totalMs = a.probeMs = a.buildMs = a.drawMs = a.maxMs = 0;
            logger().info("[arm-gpu] bind world={} resourceSerial={} source=renderer-handle revision={} layout={} image={}x{} upload=false",
                world, resourceSerial, source.revision, a.slim ? "slim" : "wide", width, height);
        }
        ensureArmMeshes(screen, a);
        return;
    }
    SkinPixels skin;
    skin.reason = reason;
    skin.slim = source.geometry == "geometry.humanoid.customSlim";
    if (a.textureReady && a.location && a.fallback && a.slim == skin.slim) {
        ensureArmMeshes(screen, a);
        return; // unchanged fallback needs neither PNG I/O nor another upload
    }
    if (skin.rgba.empty() && !readDefault(skin)) {
        // Do not retain a previous user's skin if both current and default are unavailable.
        dropTexture(a);
        if (!a.missingLogged) logger().warn("[arm-gpu] skin/default unavailable; arms deferred ({})", skin.reason);
        a.missingLogged = true;
        return;
    }
    model::makeArmSkinBaseOpaque(skin.rgba, skin.width, skin.height);
    auto hash = model::armSkinHash(skin.rgba);
    bool same = a.location && hash == a.hash && skin.width == a.width
        && skin.slim == a.slim && a.fallback;
    if (same) { ensureArmMeshes(screen, a); return; }
    a.frames = a.builds = a.draws = 0;
    a.totalMs = a.probeMs = a.buildMs = a.drawMs = a.maxMs = 0;
    auto start = Clock::now();
    dropTexture(a);
    auto group = client.getTextureGroup(); // already authorized by the caller's resource key
    if (!group) return;
    auto location = std::make_unique<ResourceLocation>(Core::PathView(fmt::format(
        "tacz/runtime/{}/{}/arms/{}", world, resourceSerial, ++skinSerial)), ResourceFileSystem::Raw);
    mce::Image image(skin.width, skin.height, mce::ImageFormat::RGBA8Unorm, mce::ImageUsage::SRGB);
    image.mAlphaUsage = mce::AlphaUsage::Transparent;
    image.setRawImage(mce::Blob(skin.rgba.data(), skin.rgba.size()));
    auto& texture = group->uploadTexture(*location, cg::ImageBuffer(std::move(image)));
    a.texture = mce::TexturePtr(texture, *location, cg::TextureSetLayerType::Color);
    a.location = std::move(location);
    a.owner = group;
    a.hash = hash;
    a.width = skin.width;
    a.slim = skin.slim;
    a.fallback = skin.fallback;
    a.textureReady = true;
    a.missingLogged = false;
    a.buildMs += msSince(start);
    ensureArmMeshes(screen, a);
    logger().info("[arm-gpu] create world={} resourceSerial={} skinSerial={} layout={} source={} reason={} image={}x{} (cached meshes, matrix-only animation)",
        world, resourceSerial, skinSerial, a.slim ? "slim" : "wide", a.fallback ? "default" : "local-skin",
        skin.reason, skin.width, skin.height);
}

void pop(MatrixStack::MatrixStackRef& ref) {
    ref.stack->_isDirty = true;
    if (ref.stack->sortOrigin->has_value() && ref.stack->stack->size() - 1 <= ref.stack->sortOrigin->value())
        ref.stack->sortOrigin->reset();
    ref.stack->stack->pop_back();
    ref.mat = nullptr;
    ref.stack = nullptr;
}
} // namespace

void releasePlayerArms(char const* reason) {
    if (!cache) return;
    for (auto& mesh : cache->meshes) mesh.reset();
    dropTexture(*cache);
    cache.reset();
    logger().info("[arm-gpu] release reason={}", reason);
}

void drawPlayerArms(ScreenContext& screen, IClientInstance& client, LocalPlayer& player,
    model::GeoModel const& geo, ArmPaths const& paths, animation::Pose const& pose,
    uint64_t world, uint64_t resourceSerial, bool rtx) {
    if (paths[0].empty() && paths[1].empty()) return;
    auto start = Clock::now();
    if (!cache) cache = std::make_unique<ArmCache>();
    auto probeStart = Clock::now();
    probeSkin(screen, client, player, world, resourceSerial);
    auto& a = *cache;
    if (a.frames < 120) a.probeMs += msSince(probeStart);
    if (!a.textureReady) return;
    auto* light = client.getLightTexture();
    auto& dispatch = client.getBlockEntityRenderDispatcher();
    auto* moving = static_cast<MovingBlockActorRenderer*>(dispatch.mRenderers.get()[BlockActorRendererId::MovingBlock].get());
    if (!light || !moving) return;
    if (!a.materials[0]) a.materials[0].emplace(moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerAlphatest)].get());
    if (!a.materials[1]) a.materials[1].emplace(moving->mBlockMaterials[static_cast<int>(BlockRenderLayer::RenderlayerBlend)].get());
    if (!a.materialLogged) {
        logger().info("[arm-gpu] materials baseValid={} sleeveValid={} (alphatest base / blended sleeve)",
            static_cast<bool>(a.materials[0]->mRenderMaterialInfoPtr),
            static_cast<bool>(a.materials[1]->mRenderMaterialInfoPtr));
        a.materialLogged = true;
    }
    BrightnessPair full;
    full.sky->mValue = 15;
    full.block->mValue = 15;
    ActorShaderManager::setupShaderParameters(screen, player.getDimensionBlockSource(), full,
        glm::vec4{1,1,1,1}, 1, true, *light, Vec2{1,1}, Vec4{0,0,1,1});
    auto drawStart = Clock::now();
    for (int side = 0; side < 2; ++side) {
        auto transform = model::playerArmTransform(geo, paths[side], pose);
        if (!transform) continue;
        auto worldMatrix = screen.camera.worldMatrixStack->push(false);
        worldMatrix.stack->_isDirty = true;
        worldMatrix.mat->_m = worldMatrix.mat->_m.get() * *transform;
        try {
            for (int layer = 0; layer < 2; ++layer) {
                auto const& mat = a.materials[layer];
                auto const& mesh = a.meshes[side * 2 + layer];
                if (!mat || !mat->mRenderMaterialInfoPtr || !mesh || !mesh->isValid()) continue;
                mesh->renderMesh(screen, *mat, a.texture, 0, 24, OffscreenCaptureDescription{}, nullptr);
                if (!a.skinDrawLogged) {
                    logger().info("[arm-skin-draw] world={} resourceSerial={} source={} revision={} fallbackSerial={} ownedDefault={} variant={} borrowedBlock={:#x} draw-returned=true selection='{}'",
                        world, resourceSerial, a.fallback ? "default" : "renderer-handle", a.borrowedRevision,
                        a.fallback ? skinSerial : 0, static_cast<bool>(a.location), a.texture.index(), a.borrowedBlock,
                        a.skinLogKey);
                    a.skinDrawLogged = true;
                }
                ++a.draws;
            }
        } catch (...) { pop(worldMatrix); throw; } // stack ownership, not exception suppression
        pop(worldMatrix);
    }
    if (a.frames < 120) {
        a.drawMs += msSince(drawStart);
        double elapsed = msSince(start);
        a.totalMs += elapsed;
        a.maxMs = std::max(a.maxMs, elapsed);
        if (++a.frames == 120) logger().info(
            "[arm-perf] rtx={} 120 frames: CPU totalMean={:.3f} max={:.3f} probeMean={:.3f} buildUploadTotal={:.3f} matrixDrawMean={:.3f} ms builds={} draws={} gpu-not-timed",
            rtx, a.totalMs / 120, a.maxMs, a.probeMs / 120, a.buildMs, a.drawMs / 120, a.builds, a.draws);
    }
}

} // namespace tacz::client::render
