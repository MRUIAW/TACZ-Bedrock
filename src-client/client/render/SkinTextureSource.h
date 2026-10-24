#pragma once

#include <cstdint>
#include <memory>
#include <string>

class LocalPlayer;
struct BedrockTextureData;

namespace tacz::client::render {

// The observer keeps only weak identities/texture references and scalar diagnostics, even
// when native skins update in the dressing room. No Player/client/level/global GPU lookup.
void enableSkinTextureSource();
void disableSkinTextureSource();

struct RenderSkinTexture {
    std::weak_ptr<void> skinOwner;
    std::shared_ptr<BedrockTextureData const> texture; // only returned to an authorized world draw
    std::string geometry;
    uint64_t revision = 0;
    uint64_t updatesSeen = 0;
    // Snapshot addresses for correlation only; never dereferenced later. Shared-owner
    // equality, not an address comparison, still decides which record matches.
    uintptr_t localIdentity = 0, inputIdentity = 0, storedIdentity = 0, infoIdentity = 0;
    uintptr_t textureIdentityAtUpdate = 0, handleIdentityAtUpdate = 0;
    char const* matchKind = "none";
    uint32_t updateThread = 0;
    uint32_t inputWidth = 0, inputHeight = 0, inputDepth = 0, inputFormat = 0;
    size_t inputBytes = 0;
    bool imageProvided = false;
    bool refPresent = false, ownerPresent = false, ownerInitialized = false, ownerThreadMatches = false;
    size_t ownerThreadHash = 0, renderThreadHash = 0;
    char const* ownerState = "skin-ref-missing";
};

// Caller has already checked the world/session/resource key. Never dereferences mObject.
RenderSkinTexture findRenderSkinTexture(LocalPlayer& player);

} // namespace tacz::client::render
