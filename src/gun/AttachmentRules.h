#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace tacz::gun {

using AllowedAttachmentTypes = std::vector<std::optional<std::string>>;
// Same read-only typed view as the animation NBT accessor: mode 0 = tag type,
// mode 1 = primitive value; missing/wrong traversal returns null.
using NbtReader = std::function<nlohmann::json(std::vector<std::string> const&, std::string const&, int)>;

struct AttachmentTypeNames { std::string_view constant, serialized; };
inline constexpr std::array<AttachmentTypeNames, 7> AttachmentTypes{{
    {"SCOPE", "scope"}, {"MUZZLE", "muzzle"}, {"STOCK", "stock"},
    {"GRIP", "grip"}, {"LASER", "laser"}, {"EXTENDED_MAG", "extended_mag"}, {"NONE", "NONE"}
}};
inline constexpr std::string_view EmptyAttachment = "tacz:empty";

// ResourceLocation.tryParse uses default namespace for absent/empty namespace,
// and permits an empty path. ResourceId::parse is deliberately stricter; do not
// widen that project-wide parser for this NBT-only Java compatibility boundary.
inline std::optional<std::string> parseAttachmentId(std::string_view text) {
    auto colon = text.find(':');
    auto ns = colon == std::string_view::npos || colon == 0 ? std::string_view{"minecraft"} : text.substr(0, colon);
    auto path = colon == std::string_view::npos ? text : text.substr(colon + 1);
    auto validNamespace = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    };
    for (char c : ns) if (!validNamespace(c)) return std::nullopt;
    for (char c : path) if (!validNamespace(c) && c != '/') return std::nullopt;
    return std::string(ns) + ':' + std::string(path);
}

// AttachmentType.valueOf is case-sensitive for API arguments. The allow-list
// uses Gson's exact serialized names plus enum.toString() fallback names.
// Unknown gun data is nullptr; absent and explicit-null lists both disallow all.
inline std::string getAttachment(AllowedAttachmentTypes const* allowed, NbtReader const& read, std::string_view type) {
    auto empty = [] { return std::string(EmptyAttachment); };
    if (!allowed) return empty();
    AttachmentTypeNames const* names = nullptr;
    for (auto const& entry : AttachmentTypes) if (type == entry.constant) { names = &entry; break; }
    if (!names) return empty();
    bool permitted = false;
    for (auto const& raw : *allowed) {
        if (raw && (*raw == names->serialized || *raw == names->constant)) { permitted = true; break; }
    }
    if (!permitted || !read) return empty();
    auto key = "Attachment" + std::string(type);
    std::vector<std::string> path{key};
    if (read({}, key, 0) != 10 || read(path, "tag", 0) != 10) return empty();
    path.push_back("tag");
    if (read(path, "AttachmentId", 0) != 8) return empty();
    auto value = read(path, "AttachmentId", 1);
    if (!value.is_string()) return empty();
    return parseAttachmentId(value.get_ref<std::string const&>()).value_or(std::string(EmptyAttachment));
}

} // namespace tacz::gun
