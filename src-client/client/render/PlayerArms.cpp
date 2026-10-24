#include "client/render/PlayerArms.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <thread>
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
#include "mc/deps/minecraft_renderer/renderer/MaterialPtr.h"
#include "mc/deps/minecraft_renderer/renderer/Mesh.h"
#include "mc/deps/minecraft_renderer/renderer/TexturePtr.h"
#include "mc/deps/minecraft_renderer/resources/ClientTexture.h"
#include "mc/deps/minecraft_renderer/resources/ServerTexture.h"
#include "mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h"
#include "mc/deps/renderer/Camera.h"
#include "mc/deps/renderer/MatrixStack.h"
#include "mc/world/actor/player/AnimatedImageData.h"
#include "mc/world/actor/player/SerializedSkinImpl.h"
#include "mc/world/actor/player/SerializedSkinRef.h"
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

// Read only on the skin's owning thread. Hold the shared wrapper only for this read;
// no Player, SerializedSkinRef, native image or registry-bound object survives the call.
SkinPixels readSkin(LocalPlayer& player) {
    SkinPixels out;
    auto const& ref = player.mSkin.get();
    if (!ref) { out.reason = "skin-not-ready"; return out; }
    auto owner = ref->mSkinImpl.get();
    if (!owner || !owner->mThreadIdInitialized || owner->mThreadId != std::this_thread::get_id()) {
        out.reason = "skin-owner-thread-unavailable";
        return out;
    }
    auto const& skin = owner->mObject;
    auto arm = skin.mArmSizeType;
    auto const& geometry = skin.mDefaultGeometryName.get();
    out.slim = arm == SharedTypes::persona::ArmSizeType::Slim
        || geometry == "geometry.humanoid.customSlim" || geometry == "geometry.humanoid.slim";
    auto const& image = skin.mSkinImage.get();
    if (image.imageFormat != mce::ImageFormat::RGBA8Unorm || image.mDepth > 1
        || !model::isClassicArmSkin(image.mWidth, image.mHeight, image.mImageBytes.size(),
            skin.mIsPersona, !skin.mSkinAnimatedImages->empty(), geometry)) {
        out.reason = skin.mIsPersona ? "persona" : "nonstandard-or-animated-skin";
        return out;
    }
    out.width = image.mWidth;
    out.height = image.mHeight;
    out.rgba.assign(image.mImageBytes.cbegin(), image.mImageBytes.cend());
    return out;
}

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
    if (a.location) {
        if (auto owner = a.owner.lock()) owner->unloadTexture(*a.location, false);
        a.location.reset();
    }
    a.owner.reset();
}

void probeSkin(ScreenContext& screen, IClientInstance& client, LocalPlayer& player,
    uint64_t world, uint64_t resourceSerial) {
    auto& a = *cache;
    auto now = Clock::now();
    if (now < a.nextProbe) return;
    a.nextProbe = now + std::chrono::seconds(1);
    auto skin = readSkin(player);
    if (skin.rgba.empty() && !readDefault(skin)) {
        // Do not retain a previous user's skin if both current and default are unavailable.
        dropTexture(a);
        if (!a.missingLogged) logger().warn("[arm-gpu] skin/default unavailable; arms deferred ({})", skin.reason);
        a.missingLogged = true;
        return;
    }
    model::makeArmSkinBaseOpaque(skin.rgba, skin.width, skin.height);
    auto hash = model::armSkinHash(skin.rgba);
    bool meshesValid = std::all_of(a.meshes.begin(), a.meshes.end(), [](auto const& mesh) {
        return mesh && mesh->isValid();
    });
    bool same = a.location && hash == a.hash && skin.width == a.width
        && skin.slim == a.slim && skin.fallback == a.fallback && meshesValid;
    if (same) return;
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
    a.missingLogged = false;
    // At most four tiny 24-vertex meshes per skin change; NEVER per pose revision.
    for (int side = 0; side < 2; ++side) for (int layer = 0; layer < 2; ++layer) {
        auto vertices = model::playerArmVertices(side == 0 ? model::Arm::Left : model::Arm::Right, a.slim, layer != 0);
        Tessellator batch(screen.tessellator.mBufferResourceService);
        batch.begin({}, mce::PrimitiveMode::QuadList, 24, false);
        batch.color(1, 1, 1, 1);
        for (auto const& vertex : vertices) {
            batch.mNextUV[0] = vertex.uv;
            batch.mNextUV[1] = glm::vec2{1, 1}; // block-material light UVs
            batch.mNextNormal = glm::vec4(vertex.normal, 0);
            batch.vertex(vertex.pos.x, vertex.pos.y, vertex.pos.z);
        }
        a.meshes[side * 2 + layer].emplace(batch.end(Tessellator::UploadMode::Buffered,
            "tacz player arm", SupplementaryFieldAutoGenerationMode{}));
        ++a.builds;
    }
    a.buildMs += msSince(start);
    logger().info("[arm-gpu] create world={} resourceSerial={} skinSerial={} layout={} source={} reason={} image={}x{} meshes=4 vertices=96 (cached, matrix-only animation)",
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
    if (!a.location) return;
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
