#pragma once
#include <string_view>
#include <nlohmann/json.hpp>

namespace tacz::pack {
// Gson JsonReader LENIENT syntax, without coercing the types of primitive values.
// Typed POJO coercion belongs to GunPackParser, not this generic tree reader.
nlohmann::json readLenientJson(std::string_view text);
}
