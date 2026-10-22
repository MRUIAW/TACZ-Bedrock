#pragma once

#include <compare>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace tacz::pack {

// Equivalent of Minecraft Java's ResourceLocation ("namespace:path").
struct ResourceId {
    std::string ns;
    std::string path;

    static constexpr std::string_view DefaultNamespace = "minecraft";

    // Parses "ns:path" or "path" (default namespace). Returns nullopt on invalid characters,
    // following ResourceLocation's rules: [a-z0-9_.-] for namespace, plus '/' for path.
    static std::optional<ResourceId> parse(std::string_view text) {
        ResourceId id;
        auto       colon = text.find(':');
        if (colon == std::string_view::npos) {
            id.ns   = DefaultNamespace;
            id.path = text;
        } else {
            id.ns   = text.substr(0, colon);
            id.path = text.substr(colon + 1);
        }
        if (id.ns.empty() || id.path.empty()) return std::nullopt;
        for (char c : id.ns) {
            if (!validNamespaceChar(c)) return std::nullopt;
        }
        for (char c : id.path) {
            if (!validPathChar(c)) return std::nullopt;
        }
        return id;
    }

    [[nodiscard]] std::string toString() const { return ns + ':' + path; }

    auto operator<=>(ResourceId const&) const = default;

private:
    static constexpr bool validNamespaceChar(char c) {
        return c == '_' || c == '-' || c == '.' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    }
    static constexpr bool validPathChar(char c) { return validNamespaceChar(c) || c == '/'; }
};

} // namespace tacz::pack

template <>
struct std::hash<tacz::pack::ResourceId> {
    size_t operator()(tacz::pack::ResourceId const& id) const noexcept {
        return std::hash<std::string>{}(id.ns) * 31 + std::hash<std::string>{}(id.path);
    }
};
