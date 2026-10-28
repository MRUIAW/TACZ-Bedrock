#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <set>

#include "pack/GunData.h"
#include "pack/GunDisplay.h"
#include "pack/ResourceId.h"
#include "pack/PackItems.h"

// Discovers TACZ gun packs in a folder (same layout as Java's .minecraft/tacz)
// and loads their common data. Mirrors GunPackLoader + CommonAssetsManager.
namespace tacz::pack {

// Read-only view of one gun pack (folder today; zip later).
class PackSource {
public:
    virtual ~PackSource() = default;

    [[nodiscard]] virtual std::string const& name() const = 0;
    // Paths relative to the pack root using '/', under the given prefix (e.g. "data/").
    [[nodiscard]] virtual std::vector<std::string> list(std::string_view prefix) const = 0;
    [[nodiscard]] virtual std::optional<std::string> read(std::string_view path) const = 0;
    // Separate discovery from opening: a selected but unreadable file must not
    // silently fall back to a lower-priority source.
    [[nodiscard]] virtual bool contains(std::string_view path) const { return read(path).has_value(); }
};

std::unique_ptr<PackSource> openFolderPack(std::filesystem::path const& root);

struct GunEntry {
    ResourceId   id;
    GunIndexData index;
    GunData      data;
    std::string  packName;
};

struct AmmoEntry {
    ResourceId    id;
    AmmoIndexData index;
    std::string   packName;
};

struct LoadedPack {
    std::string name;
    PackMeta    meta;
};

// Retrieval operation, not file extension: Java's direct and listing APIs can
// deliberately select different contents for the very same path.
enum class ResourceLookup { Direct, Listed, ListedStack, ResourceStack };
struct ResolvedResource {
    std::string pack;
    std::optional<std::string> bytes; // selected supplier may fail to open
    bool builtin=false;
};

class GunPackRegistry {
public:
    using LogFn = std::function<void(std::string const&)>;

    // Scans `folder` for sub-folders containing gunpack.meta.json, then loads every pack.
    // Problems are reported through `warn` and the offending entry is skipped, like TACZ does.
    void loadFolder(std::filesystem::path const& folder, LogFn const& info, LogFn const& warn);

    // Loads already-opened packs; later packs override earlier ones on id clashes.
    // The registry keeps the packs open so assets can be read on demand.
    void load(std::vector<std::unique_ptr<PackSource>> packs, LogFn const& info, LogFn const& warn);

    void clear();

    [[nodiscard]] GunEntry const*   gun(ResourceId const& id) const;
    [[nodiscard]] AmmoEntry const*  ammo(ResourceId const& id) const;
    [[nodiscard]] GunDisplay const* gunDisplay(ResourceId const& id) const;
    [[nodiscard]] std::optional<ResourceId> ammoSlot(ResourceId const& id) const {
        auto it=mAmmoSlots.find(id); return it==mAmmoSlots.end() ? std::nullopt : std::optional(it->second);
    }
    [[nodiscard]] std::string localize(std::string const& key,std::string const& locale) const { return lookupTranslation(mLanguages,key,locale); }
    [[nodiscard]] std::string packDisplayName(ResourceId const& id,std::string const& locale) const;

    // Reads a file by pack-relative path (e.g. "assets/tacz/textures/gun/uv/ak47.png");
    // the last pack containing it wins, as with resource pack stacking.
    [[nodiscard]] std::optional<std::string> readFile(std::string_view path) const;
    [[nodiscard]] std::optional<std::string> readFile(std::string_view path,ResourceLookup mode) const;
    [[nodiscard]] std::vector<ResolvedResource> resolve(std::string_view path,ResourceLookup mode) const;
    [[nodiscard]] bool hasFile(std::string const& path) const;
    // Per-gun startup audit, aggregated once (including clip-keyed sounds).
    [[nodiscard]] std::map<ResourceId,std::vector<std::string>> const& assetIssues() const { return mAssetIssues; }

    [[nodiscard]] std::map<ResourceId, GunEntry> const&  guns() const { return mGuns; }
    [[nodiscard]] std::map<ResourceId, AmmoEntry> const& ammos() const { return mAmmo; }
    [[nodiscard]] std::vector<LoadedPack> const&         packs() const { return mPacks; }

    // Lua sources found under data/<ns>/scripts/*.lua, keyed by id (e.g. tacz:xmag_reload_logic).
    [[nodiscard]] std::map<ResourceId, std::string> const& dataScripts() const { return mDataScripts; }
    [[nodiscard]] std::map<ResourceId, std::string> const& assetScripts() const { return mAssetScripts; }

private:
    std::vector<std::unique_ptr<PackSource>> mSources; // packs with a valid gunpack.meta.json
    std::vector<LoadedPack>                  mPacks;
    std::map<ResourceId, GunEntry>           mGuns;
    std::map<ResourceId, AmmoEntry>          mAmmo;
    std::map<ResourceId, GunDisplay>         mGunDisplays;
    std::map<ResourceId,ResourceId>          mAmmoSlots;
    LanguageTable                          mLanguages;
    std::map<std::string,std::string>       mPackDisplayNames; // namespace -> gunpack_info.name translation key
    std::map<ResourceId, std::string>        mDataScripts;
    std::map<ResourceId, std::string>        mAssetScripts;
    std::set<std::string,std::less<>>        mAssetPaths;
    std::map<ResourceId,std::vector<std::string>> mAssetIssues;
};

// CommonGunIndex.checkIndex / checkData. Returns an error message, or empty if valid.
// Also fills inaccuracy defaults in place, as TACZ does.
std::string validateGun(GunIndexData const& index, GunData& data);

} // namespace tacz::pack
