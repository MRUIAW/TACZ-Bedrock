#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "pack/GunData.h"

// JSON -> data model, mirroring TACZ's Gson setup (CommonAssetsManager.GSON):
// comments allowed, unknown keys ignored, unknown enum names become "unset",
// a field of the wrong type fails the whole file.
namespace tacz::pack {

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Parses JSON text with // and /* */ comments. Throws ParseError.
nlohmann::json parseJsonText(std::string_view text);

PackMeta      parsePackMeta(nlohmann::json const& json);
GunData       parseGunData(nlohmann::json const& json);
GunIndexData  parseGunIndex(nlohmann::json const& json);
AmmoIndexData parseAmmoIndex(nlohmann::json const& json);

} // namespace tacz::pack
