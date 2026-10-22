add_rules("mode.debug", "mode.release")

-- Single source of the mod version. Keep tooth.json "version" in sync.
-- Patch +1 per small item / test round; minor +1 per finished feature or phase.
local tacz_version = "0.2.0"
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
add_requires("levilamina 26.51.7", {configs = {target_type = get_config("target_type")}})

add_requires("levibuildscript")

-- Unit tests for game-independent logic (src/pack, src/gun). Not built by default:
--   xmake build tacz-tests && xmake run tacz-tests
add_requires("doctest 2.5.3", "nlohmann_json v3.12.0")

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("tacz")
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
    add_packages("levilamina")
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
        add_headerfiles("src-client/**.h")
        add_includedirs("src-client")
        add_files("src-client/**.cpp")
    end

target("tacz-tests")
    set_default(false)
    set_kind("binary")
    set_languages("c++20")
    if is_plat("windows") then
        add_defines("NOMINMAX", "UNICODE")
        add_cxflags("/EHsc", "/utf-8")
        set_toolchains("clang-cl")
    end
    add_packages("doctest", "nlohmann_json")
    add_includedirs("src")
    -- Only sources that do not touch the game or LeviLamina.
    add_files("src/pack/**.cpp", "tests/**.cpp")
    -- The reference TACZ checkout supplies the default gun pack for integration tests.
    add_defines(format('TACZ_DEFAULT_PACK_DIR="%s"',
        path.join(os.projectdir(), "_reference/TACZ/src/main/resources/assets/tacz/custom/tacz_default_gun"):gsub("\\", "/")))
