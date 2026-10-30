#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <sol/sol.hpp>

namespace tacz::lua {

// FiguraMC LuaJ LuaString.scannumber/scanlong/scandouble. Non-numbers coerce
// to zero; only ASCII spaces are trimmed, and decimal fallback scans 64 bytes.
inline double luaJDouble(sol::object const& value) {
    if (value.get_type() == sol::type::number) return value.as<double>();
    if (value.get_type() != sol::type::string) return 0;
    auto s = value.as<std::string>();
    auto first = s.find_first_not_of(' '), last = s.find_last_not_of(' ');
    if (first == std::string::npos) return 0;
    s = s.substr(first, last - first + 1);
    bool hex = s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    auto digits = hex ? s.substr(2) : s;
    if (digits.empty()) return 0;
    bool negative = digits[0] == '-';
    uint64_t n = 0;
    bool valid = true;
    for (size_t i = negative ? 1 : 0; i < digits.size(); ++i) {
        unsigned char c = digits[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : c - 'a' + 10;
        if (d < 0 || d >= (hex ? 16 : 10)) { valid = false; break; }
        n = n * (hex ? 16 : 10) + d;
        if (n >> 63) { valid = false; break; }
    }
    if (valid) return negative ? -static_cast<double>(n) : static_cast<double>(n);
    if (hex) return 0;
    s.resize(std::min<size_t>(s.size(), 64));
    if (s.find_first_not_of("-+.eE0123456789") != std::string::npos) return 0;
    char* end = nullptr;
    double d = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && !std::isnan(d) ? d : 0;
}

// Java double -> long saturates/NaN->0; LuaDouble.toint then narrows long
// to the low 32 bits (not a direct saturating double -> int conversion).
inline int64_t luaJLong(sol::object const& value) {
    double d = luaJDouble(value);
    if (std::isnan(d)) return 0;
    if (d >= 0x1p63) return std::numeric_limits<int64_t>::max();
    if (d <= -0x1p63) return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(d);
}
inline int luaJInt(sol::object const& value) {
    return std::bit_cast<int32_t>(static_cast<uint32_t>(luaJLong(value)));
}

} // namespace tacz::lua
