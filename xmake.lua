add_rules("mode.debug", "mode.release")

-- Single source of the mod version. Keep tooth.json "version" in sync.
-- Patch +1 per small item / test round; minor +1 per finished feature or phase.
local tacz_version = "0.4.8"
set_version(tacz_version)

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("server")
    set_showmenu(true)
    set_values("server", "client")
option_end()

-- add_requires("levilamina x.x.x") for a specific version
-- add_requires("levilamina develop") to use develop version
-- please note that you should add bdslibrary yourself if using dev version
add_requires("levilamina 26.51.9", {configs = {target_type = get_config("target_type")}})

add_requires("levibuildscript")

-- Unit tests for game-independent logic (src/pack, src/gun). Not built by default:
--   xmake build tacz-tests && xmake run tacz-tests
add_requires("doctest 2.5.3", "nlohmann_json v3.12.0", "glm 1.0.1")
-- PNG decoding for gun pack textures (client).
add_requires("stb 2025.03.14")
-- Gun pack Lua scripts (TACZ uses LuaJ, Lua 5.2 semantics; the default pack runs on 5.4).
add_requires("lua v5.4.8", {configs = {shared = false}})
add_requires("sol2 v3.5.0", {configs = {includes_lua = false}})

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

-- TACZ's internal fallback assets, embedded so deployed mods do not need the reference tree.
rule("tacz.builtin_assets")
    on_load(function (target)
        local dir = path.join(os.projectdir(), "build/generated")
        local text = "#pragma once\n#include <cstddef>\nnamespace tacz::pack::builtin {\n"
        for _, name in ipairs({"rifle_default", "pistol_default", "default_state_machine"}) do
            local ext = name == "default_state_machine" and ".lua" or ".animation.json"
            text = text .. "inline constexpr char " .. name .. "[] = R\"tacz_asset("
                .. io.readfile(path.join(os.projectdir(), "resources/tacz", name .. ext)) .. ")tacz_asset\";\n"
        end
        for _, code in ipairs({"en_us", "zh_cn"}) do
            text = text .. "inline constexpr char lang_" .. code .. "[] = R\"tacz_asset("
                .. io.readfile(path.join(os.projectdir(), "resources/tacz/lang", code .. ".json")) .. ")tacz_asset\";\n"
        end
        local images = os.files(path.join(os.projectdir(), "resources/tacz/textures/**.png"))
        table.sort(images)
        for i, file in ipairs(images) do
            local bytes = assert(io.readfile(file, {encoding = "binary"}))
            text = text .. "inline constexpr unsigned char image_" .. i .. "[] = {"
            for k = 1, #bytes do text = text .. tostring(bytes:byte(k)) .. "," end
            text = text .. "};\n"
        end
        text = text .. "struct Image { char const* path; unsigned char const* data; std::size_t size; };\ninline constexpr Image images[] = {\n"
        for i, file in ipairs(images) do
            local relative = path.relative(file, path.join(os.projectdir(), "resources/tacz")):gsub("\\", "/")
            text = text .. '{"assets/tacz/' .. relative .. '",image_' .. i .. ',sizeof(image_' .. i .. ')},\n'
        end
        text = text .. "};\n}\n"
        local output = path.join(dir, "TaczBuiltinAssets.h")
        if not os.isfile(output) or io.readfile(output) ~= text then
            os.mkdir(dir)
            io.writefile(output, text)
        end
        target:add("includedirs", dir)
    end)
rule_end()

target("tacz")
    add_rules("tacz.builtin_assets")
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker", {modVersion = tacz_version})
    add_defines(format('TACZ_VERSION="%s"', tacz_version))
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE", "_HAS_CXX23=1")
        set_exceptions("none") -- To avoid conflicts with /EHa.
        add_cxflags( "/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
        add_cxflags(
            "/EHs",
            "-Wno-microsoft-cast",
            "-Wno-invalid-offsetof",
            "-Wno-c++2b-extensions",
            "-Wno-microsoft-include",
            "-Wno-overloaded-virtual",
            "-Wno-ignored-qualifiers",
            "-Wno-missing-field-initializers",
            "-Wno-potentially-evaluated-expression",
            "-Wno-pragma-system-header-outside-header",
            {tools = {"clang_cl"}}
        )
        set_toolchains("clang-cl")
    end
    add_packages("levilamina", "lua", "sol2")
    set_kind("shared")
    set_languages("c++20")
    set_symbols("debug")
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")
    -- Server logic is built into both variants: the client hosts an integrated server for
    -- single-player / LAN worlds (same as Java's integrated server), and stays inert when
    -- connected to a remote BDS.
    add_headerfiles("src-server/**.h")
    add_includedirs("src-server")
    add_files("src-server/**.cpp")
    if is_config("target_type", "server") then
        add_defines("TACZ_SERVER")
    else
        add_defines("TACZ_CLIENT")
        add_packages("stb")
        add_syslinks("user32") -- GetKeyState for key modifiers
        add_headerfiles("src-client/**.h")
        add_includedirs("src-client")
        add_files("src-client/**.cpp")
    end

target("tacz-tests")
    add_rules("tacz.builtin_assets")
    set_default(false)
    set_kind("binary")
    set_languages("c++20")
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE")
        add_cxflags("/EHsc", "/utf-8")
        set_toolchains("clang-cl")
    end
    add_packages("doctest", "nlohmann_json", "glm", "lua", "sol2")
    add_includedirs("src")
    -- Only sources that do not touch the game or LeviLamina.
    add_files("src/pack/**.cpp", "src/model/**.cpp", "src/lua/**.cpp", "tests/**.cpp")
    add_defines(format('TACZ_PROJECT_DIR="%s"', os.projectdir():gsub("\\", "/")))
    -- The reference TACZ checkout supplies the default gun pack for integration tests.
    add_defines(format('TACZ_DEFAULT_PACK_DIR="%s"',
        path.join(os.projectdir(), "_reference/TACZ/src/main/resources/assets/tacz/custom/tacz_default_gun"):gsub("\\", "/")))

target("tacz-pack-audit")
    add_rules("tacz.builtin_assets")
    set_default(false)
    set_kind("binary")
    set_languages("c++20")
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE")
        add_cxflags("/EHsc", "/utf-8")
        set_toolchains("clang-cl")
    end
    add_packages("nlohmann_json", "glm")
    add_includedirs("src")
    add_files("src/pack/**.cpp", "src/model/**.cpp", "scripts/pack-audit.cpp")

-- Read-only production resolution/VM report for arbitrary external gun packs.
-- No installed pack or expected winner is a unit-test dependency.
target("tacz-pack-resolve")
    add_rules("tacz.builtin_assets")
    set_default(false)
    set_kind("binary")
    set_languages("c++20")
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE")
        add_cxflags("/EHsc", "/utf-8")
        set_toolchains("clang-cl")
    end
    add_packages("nlohmann_json", "glm", "lua", "sol2")
    add_includedirs("src")
    add_files("src/pack/**.cpp", "src/model/**.cpp", "src/lua/**.cpp", "scripts/pack-resolve.cpp")
