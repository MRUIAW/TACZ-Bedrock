#pragma once

#include <sol/sol.hpp>

namespace tacz::lua {

// FiguraMC LuaJ CoerceLuaToJava.BoolCoercion: only nil/false are false;
// numbers (including 0), strings and tables are true. For Java boolean
// PARAMETERS only: hook return values still use strict checkboolean rules.
inline bool luaJBoolean(sol::object const& value) {
    return value.valid() && (value.get_type() != sol::type::boolean || value.as<bool>());
}

} // namespace tacz::lua
