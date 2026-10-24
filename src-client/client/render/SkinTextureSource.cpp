#include "client/render/SkinTextureSource.h"

#include <algorithm>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include <Windows.h>

#include "ll/api/io/Logger.h"
#include "ll/api/memory/Hook.h"
#include "mc/client/persona/ClientSkinInfoData.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/deps/minecraft_renderer/renderer/BedrockTextureData.h"
#include "mc/world/actor/player/SerializedSkinImpl.h"
#include "client/gameplay/ClientShooter.h"
#include "mod/TaczMod.h"

namespace tacz::client::render {
namespace {
using UpdateSkinSignature = void (ClientSkinInfoData::*)(SerializedSkinRef const&, mce::Image const*, mce::Image const*);
static_assert(std::is_same_v<decltype(&ClientSkinInfoData::updateSkin), UpdateSkinSignature>);
static_assert(std::is_same_v<decltype(&ClientSkinInfoData::$updateSkin), UpdateSkinSignature>);

struct ObservedSkin {
    std::weak_ptr<void> inputOwner, storedOwner;
    std::weak_ptr<BedrockTextureData const> texture;
    std::string geometry;
    uint64_t revision;
    uint32_t thread, width, height, depth, format;
    size_t bytes;
    bool imageProvided;
    uintptr_t inputIdentity, storedIdentity, infoIdentity, textureIdentity, handleIdentity;
};
std::mutex observedMutex;
std::vector<ObservedSkin> observed;
uint64_t nextRevision = 0;
bool hooked = false;

bool sameOwner(std::weak_ptr<void> const& a, std::shared_ptr<void> const& b) {
    return !a.expired() && !a.owner_before(b) && !b.owner_before(a);
}

// Actual engine update boundary, all arguments references/pointers; no opaque value ABI.
// This is an update boundary, NOT proof that a texture captured here is the final renderer
// binding or that mDefaultMeshName contains the skin's actual geometry (it can be empty).
LL_TYPE_INSTANCE_HOOK(ObserveClientSkinTexture, ll::memory::HookPriority::Normal, ClientSkinInfoData,
    &ClientSkinInfoData::$updateSkin, void, SerializedSkinRef const& skin,
    mce::Image const* image, mce::Image const* cape) {
    std::shared_ptr<void> inputOwner = skin.mSkinImpl;
    uint32_t width = image ? image->mWidth : 0, height = image ? image->mHeight : 0;
    uint32_t depth = image ? image->mDepth : 0;
    uint32_t format = image ? static_cast<uint32_t>(image->imageFormat) : 0;
    size_t bytes = image ? image->mImageBytes.size() : 0;
    origin(skin, image, cape);
    std::shared_ptr<void> storedOwner = mSkin->mSkinImpl;
    auto data = mSkinTexture->mBedrockTextureData;
    auto inputIdentity = reinterpret_cast<uintptr_t>(inputOwner.get());
    auto storedIdentity = reinterpret_cast<uintptr_t>(storedOwner.get());
    auto infoIdentity = reinterpret_cast<uintptr_t>(this);
    auto textureIdentity = reinterpret_cast<uintptr_t>(data.get());
    auto handleIdentity = data ? reinterpret_cast<uintptr_t>(data->mClientTexture->mResourcePointerBlock.get()) : 0;
    std::lock_guard lock(observedMutex);
    std::erase_if(observed, [](auto const& entry) {
        return entry.inputOwner.expired() && entry.storedOwner.expired();
    });
    // Replace the record for this skin, without pinning its native image or any GPU owner.
    std::erase_if(observed, [&](auto const& entry) {
        return sameOwner(entry.inputOwner, inputOwner) || sameOwner(entry.storedOwner, storedOwner);
    });
    if (observed.size() >= 128) observed.erase(observed.begin());
    observed.push_back({inputOwner, storedOwner, data,
        mDefaultMeshName.get(), ++nextRevision, GetCurrentThreadId(), width, height, depth, format, bytes, image != nullptr,
        inputIdentity, storedIdentity, infoIdentity, textureIdentity, handleIdentity});
    // Event-only, bounded diagnostics: no global client/level query at menu. worldToken is
    // only an atomic session token; zero means no authorized world, not necessarily main menu.
    if (nextRevision <= 128) {
        TaczMod::getInstance().getSelf().getLogger().info(
            "[arm-skin-update] event={} worldToken={} thread={} info={:#x} inputOwner={:#x} storedOwner={:#x} sameOwner={} nativeValid={} data={:#x} block={:#x} loadedState={} missingState={} gpu={}x{} defaultMesh='{}' imageProvided={} input={}x{} format={} bytes={}",
            nextRevision, shooter::worldToken(), GetCurrentThreadId(), infoIdentity, inputIdentity, storedIdentity,
            !inputOwner.owner_before(storedOwner) && !storedOwner.owner_before(inputOwner), static_cast<bool>(mValid),
            textureIdentity, handleIdentity, data ? static_cast<int>(data->mTextureLoadState) : -1,
            data ? static_cast<int>(data->mIsMissingTexture) : -1,
            data ? static_cast<uint32_t>(data->mTextureDescription->mWidth) : 0,
            data ? static_cast<uint32_t>(data->mTextureDescription->mHeight) : 0,
            mDefaultMeshName.get(), image != nullptr, width, height, format, bytes);
    } else if (nextRevision == 129) {
        TaczMod::getInstance().getSelf().getLogger().info(
            "[arm-skin-update] per-event log limit reached (128); selection counters and disable summary continue");
    }
}
} // namespace

void enableSkinTextureSource() {
    if (hooked) return;
    hooked = ObserveClientSkinTexture::hook() == 0;
    if (hooked) TaczMod::getInstance().getSelf().getLogger().info(
        "[arm-skin] native ClientSkinInfoData::updateSkin observer installed (weak-only, no menu resource queries)");
    if (!hooked) TaczMod::getInstance().getSelf().getLogger().warn(
        "[arm-skin] ClientSkinInfoData update observer unavailable; default skin fallback remains");
}

void disableSkinTextureSource() {
    if (hooked) ObserveClientSkinTexture::unhook();
    hooked = false;
    std::lock_guard lock(observedMutex);
    TaczMod::getInstance().getSelf().getLogger().info(
        "[arm-skin-update] observer disabled: totalEvents={} retainedWeakRecords={}", nextRevision, observed.size());
    observed.clear();
}

RenderSkinTexture findRenderSkinTexture(LocalPlayer& player) {
    RenderSkinTexture out;
    out.renderThreadHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    auto const& ref = player.mSkin.get();
    out.refPresent = static_cast<bool>(ref);
    if (!ref) return out;
    auto owner = ref->mSkinImpl;
    out.localIdentity = reinterpret_cast<uintptr_t>(owner.get());
    out.skinOwner = owner;
    out.ownerPresent = static_cast<bool>(owner);
    if (!owner) { out.ownerState = "skin-owner-missing"; return out; }
    // Diagnostic only: do not interpret an unset debug owner-thread marker as an invalid
    // renderer texture, and NEVER dereference the guarded SerializedSkinImpl::mObject.
    out.ownerInitialized = owner->mThreadIdInitialized;
    out.ownerThreadMatches = out.ownerInitialized && owner->mThreadId == std::this_thread::get_id();
    out.ownerThreadHash = out.ownerInitialized ? std::hash<std::thread::id>{}(owner->mThreadId) : 0;
    out.ownerState = !out.ownerInitialized ? "skin-owner-uninitialized"
        : out.ownerThreadMatches ? "skin-owner-current-thread" : "skin-owner-other-thread";
    std::shared_ptr<void> identity = owner;
    std::lock_guard lock(observedMutex);
    out.updatesSeen = nextRevision;
    for (auto it = observed.rbegin(); it != observed.rend(); ++it) {
        bool inputMatch = sameOwner(it->inputOwner, identity), storedMatch = sameOwner(it->storedOwner, identity);
        if (!inputMatch && !storedMatch) continue;
        out.matchKind = inputMatch ? (storedMatch ? "input+stored" : "input") : "stored";
        out.inputIdentity = it->inputIdentity;
        out.storedIdentity = it->storedIdentity;
        out.infoIdentity = it->infoIdentity;
        out.textureIdentityAtUpdate = it->textureIdentity;
        out.handleIdentityAtUpdate = it->handleIdentity;
        out.revision = it->revision;
        out.texture = it->texture.lock();
        out.geometry = it->geometry;
        out.updateThread = it->thread;
        out.inputWidth = it->width;
        out.inputHeight = it->height;
        out.inputDepth = it->depth;
        out.inputFormat = it->format;
        out.inputBytes = it->bytes;
        out.imageProvided = it->imageProvided;
        break;
    }
    return out;
}
} // namespace tacz::client::render
