#include "pack/GunPackLoader.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

#include "pack/GunPackParser.h"
#include "pack/BuiltinAssets.h"

namespace tacz::pack {
namespace {

namespace fs = std::filesystem;

std::string toUtf8(fs::path const& path) {
    auto u8 = path.generic_u8string();
    return {reinterpret_cast<char const*>(u8.data()), u8.size()};
}

class FolderPack final : public PackSource {
public:
    FolderPack(fs::path root, std::string name) : mRoot(std::move(root)), mName(std::move(name)) {}

    [[nodiscard]] std::string const& name() const override { return mName; }

    [[nodiscard]] std::vector<std::string> list(std::string_view prefix) const override {
        std::vector<std::string> out;
        std::error_code          ec;
        fs::path                 start = mRoot / fs::path(std::u8string(prefix.begin(), prefix.end()));
        if (!fs::is_directory(start, ec)) return out;
        for (fs::recursive_directory_iterator it(start, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            out.push_back(toUtf8(fs::relative(it->path(), mRoot, ec)));
        }
        std::ranges::sort(out);
        return out;
    }

    [[nodiscard]] std::optional<std::string> read(std::string_view path) const override {
        std::ifstream file(mRoot / fs::path(std::u8string(path.begin(), path.end())), std::ios::binary);
        if (!file) return std::nullopt;
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return std::move(buffer).str();
    }

private:
    fs::path    mRoot;
    std::string mName;
};

// Splits "data/<ns>/<dir>/<path>.<ext>" into an id, as FileToIdConverter does.
std::optional<ResourceId>
fileToId(std::string_view file, std::string_view root, std::string_view dir, std::string_view ext) {
    if (!file.starts_with(root)) return std::nullopt;
    file.remove_prefix(root.size());
    auto slash = file.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    auto ns   = file.substr(0, slash);
    auto rest = file.substr(slash + 1);
    if (!rest.starts_with(dir) || rest.size() <= dir.size() || rest[dir.size()] != '/') return std::nullopt;
    rest.remove_prefix(dir.size() + 1);
    if (!rest.ends_with(ext)) return std::nullopt;
    rest.remove_suffix(ext.size());
    return ResourceId::parse(std::string(ns) + ':' + std::string(rest));
}

struct RawFile {
    ResourceId  id;
    std::string pack;
    std::string text;
};

// Collects <root><ns>/<dir>/**.<ext> across packs; later packs win on clashes.
std::map<ResourceId, RawFile> collect(
    std::vector<std::unique_ptr<PackSource>> const& packs,
    std::string_view                                root,
    std::string_view                                dir,
    std::string_view                                ext,
    GunPackRegistry::LogFn const&                   warn
) {
    std::map<ResourceId, RawFile> out;
    for (auto const& pack : packs) {
        for (auto const& file : pack->list(root)) {
            if (!file.ends_with(ext)) continue;
            auto id = fileToId(file, root, dir, ext);
            if (!id) {
                // Only complain about files that are inside the directory but have an invalid name.
                if (file.find(std::string("/") + std::string(dir) + "/") != std::string::npos) {
                    warn("Skipping " + pack->name() + "/" + file + ": invalid resource name");
                }
                continue;
            }
            auto text = pack->read(file);
            if (!text) {
                warn("Cannot read " + pack->name() + "/" + file);
                continue;
            }
            out[*id] = RawFile{*id, pack->name(), std::move(*text)};
        }
    }
    return out;
}

} // namespace

std::unique_ptr<PackSource> openFolderPack(fs::path const& root) {
    return std::make_unique<FolderPack>(root, toUtf8(root.filename()));
}

std::string validateGun(GunIndexData const& index, GunData& data) {
    if (index.type.find_first_not_of(" \t\r\n") == std::string::npos) return "index object missing type field";
    if (!index.data) return "index object missing data field";
    if (!data.ammoId) return "ammo id is empty";
    if (data.ammoAmount < 1) return "ammo count must >= 1";
    if (data.extendedMagAmmoAmount && data.extendedMagAmmoAmount->size() < 3) return "extended_mag_ammo_amount size must is 3";
    if (data.rpm < 1) return "rpm count must >= 1";
    if (!data.bolt) return "bolt type is error";
    if (!data.reload.type) return "reload type is error";
    if (data.fireModes.empty()) return "fire mode is empty";
    for (auto const& mode : data.fireModes) {
        if (!mode) return "fire mode is error";
    }
    // checkInaccuracy: fill missing entries with the defaults.
    auto defaults = defaultInaccuracy();
    if (!data.inaccuracy || data.inaccuracy->empty()) {
        data.inaccuracy = defaults;
    } else {
        for (auto const& [type, value] : defaults) data.inaccuracy->try_emplace(type, value);
    }
    // checkRecoil
    for (auto const* frames : {&data.recoil.pitch, &data.recoil.yaw}) {
        for (auto const& frame : *frames) {
            if (frame.value.size() != 2) return "Recoil value's length must be 2";
            if (frame.value[0] > frame.value[1]) return "Recoil value's left must be less than right";
            if (frame.time < 0) return "Recoil time must be more than 0";
        }
    }
    return {};
}

void GunPackRegistry::clear() {
    mSources.clear();
    mPacks.clear();
    mGuns.clear();
    mAmmo.clear();
    mGunDisplays.clear();
    mDataScripts.clear();
    mAssetScripts.clear();
}

void GunPackRegistry::loadFolder(fs::path const& folder, LogFn const& info, LogFn const& warn) {
    std::vector<std::unique_ptr<PackSource>> packs;
    std::error_code                          ec;
    if (!fs::is_directory(folder, ec)) {
        fs::create_directories(folder, ec);
        if (ec) warn("Failed to create gun pack directory " + toUtf8(folder) + ": " + ec.message());
    }
    info("Start scanning for gun packs in " + toUtf8(folder));
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(it->path());
    std::ranges::sort(entries);
    for (auto const& entry : entries) {
        if (fs::is_directory(entry, ec)) {
            if (fs::is_regular_file(entry / "gunpack.meta.json", ec)) packs.push_back(openFolderPack(entry));
        } else if (entry.extension() == ".zip") {
            warn("Zip gun packs are not supported yet, skipping " + toUtf8(entry.filename()));
        }
    }
    load(std::move(packs), info, warn);
}

void GunPackRegistry::load(std::vector<std::unique_ptr<PackSource>> packs, LogFn const& info, LogFn const& warn) {
    clear();
    // gunpack.meta.json gates whether a pack is used at all.
    auto& valid = mSources;
    for (auto& pack : packs) {
        auto text = pack->read("gunpack.meta.json");
        if (!text) {
            warn("Failed to read info json: " + pack->name());
            continue;
        }
        try {
            auto meta = parsePackMeta(parseJsonText(*text));
            // TACZ checks dependencies against loaded Forge mods; there is no equivalent here,
            // so packs with dependencies are accepted and the requirement is only logged.
            if (!meta.dependencies.empty()) {
                info("- " + pack->name() + " declares mod dependencies; they are not checked on Bedrock");
            }
            info("- " + pack->name() + ", Main namespace: " + meta.ns);
            mPacks.push_back({pack->name(), std::move(meta)});
            valid.push_back(std::move(pack));
        } catch (ParseError const& e) {
            warn("Failed to read info json: " + pack->name() + ": " + e.what());
        }
    }
    info("Found " + std::to_string(mPacks.size()) + " possible gunpack(s)");

    auto parseAll = [&](std::string_view root, std::string_view dir, auto&& parse) {
        using Value = decltype(parse(std::declval<nlohmann::json const&>()));
        std::map<ResourceId, std::pair<Value, std::string>> out;
        for (auto& [id, file] : collect(valid, root, dir, ".json", warn)) {
            try {
                out.emplace(id, std::pair{parse(parseJsonText(file.text)), file.pack});
            } catch (ParseError const& e) {
                warn("Failed to load data file " + id.toString() + " (" + std::string(dir) + "): " + e.what());
            } catch (nlohmann::json::exception const& e) {
                warn("Failed to load data file " + id.toString() + " (" + std::string(dir) + "): " + e.what());
            }
        }
        return out;
    };

    for (auto& [id, file] : collect(valid, "data/", "scripts", ".lua", warn)) mDataScripts[id] = std::move(file.text);
    mAssetScripts[ResourceId{"tacz", "default_state_machine"}] = *builtinAsset("assets/tacz/scripts/default_state_machine.lua");
    for (auto& [id, file] : collect(valid, "assets/", "scripts", ".lua", warn)) mAssetScripts[id] = std::move(file.text);

    auto gunData  = parseAll("data/", "data/guns", parseGunData);
    auto gunIndex = parseAll("data/", "index/guns", parseGunIndex);
    auto ammo     = parseAll("data/", "index/ammo", parseAmmoIndex);
    // Client-side display definitions. Small, so both sides load them; only the client uses them.
    for (auto& [id, entry] : parseAll("assets/", "display/guns", parseGunDisplay)) mGunDisplays.emplace(id, std::move(entry.first));

    for (auto& [id, entry] : ammo) {
        entry.first.sort = std::clamp(entry.first.sort, 0, 65536);
        mAmmo.emplace(id, AmmoEntry{id, std::move(entry.first), std::move(entry.second)});
    }

    for (auto& [id, entry] : gunIndex) {
        auto& [index, packName] = entry;
        index.sort              = std::clamp(index.sort, 0, 65536);
        auto dataIt             = index.data ? gunData.find(*index.data) : gunData.end();
        if (index.data && dataIt == gunData.end()) {
            warn("Gun " + id.toString() + ": there is no corresponding data file");
            continue;
        }
        GunData data = index.data ? dataIt->second.first : GunData{};
        if (auto error = validateGun(index, data); !error.empty()) {
            warn("Gun " + id.toString() + ": " + error);
            continue;
        }
        if (data.script && !mDataScripts.contains(*data.script)) {
            warn("Gun " + id.toString() + ": script '" + data.script->toString() + "' not found");
        }
        if (index.display && !mGunDisplays.contains(*index.display)) {
            warn("Gun " + id.toString() + ": display '" + index.display->toString() + "' not found");
        }
        mGuns.emplace(id, GunEntry{id, std::move(index), std::move(data), std::move(packName)});
    }
    info(
        "Loaded " + std::to_string(mGuns.size()) + " gun(s), " + std::to_string(mAmmo.size()) + " ammo type(s), "
        + std::to_string(mDataScripts.size()) + " gun logic script(s)"
    );
}

GunEntry const* GunPackRegistry::gun(ResourceId const& id) const {
    auto it = mGuns.find(id);
    return it == mGuns.end() ? nullptr : &it->second;
}

AmmoEntry const* GunPackRegistry::ammo(ResourceId const& id) const {
    auto it = mAmmo.find(id);
    return it == mAmmo.end() ? nullptr : &it->second;
}

GunDisplay const* GunPackRegistry::gunDisplay(ResourceId const& id) const {
    auto it = mGunDisplays.find(id);
    return it == mGunDisplays.end() ? nullptr : &it->second;
}

std::optional<std::string> GunPackRegistry::readFile(std::string_view path) const {
    for (auto it = mSources.rbegin(); it != mSources.rend(); ++it) {
        if (auto content = (*it)->read(path)) return content;
    }
    if (auto text = builtinAsset(path)) return std::string(*text);
    return std::nullopt;
}

} // namespace tacz::pack
