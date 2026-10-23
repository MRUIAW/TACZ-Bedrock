#pragma once

#include <optional>
#include <string_view>

namespace tacz::pack {
std::optional<std::string_view> builtinAsset(std::string_view path);
}
