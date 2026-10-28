#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include "pack/GunPackLoader.h"
using namespace tacz;
namespace {
class OverrideSource final : public pack::PackSource {
public:
    std::string label;
    std::map<std::string,std::string> files;
    std::string unreadable;
    OverrideSource(std::string name,std::map<std::string,std::string> values):label(std::move(name)),files(std::move(values)) {
        files["gunpack.meta.json"]=R"({"namespace":"fixture"})";
    }
    std::string const& name() const override { return label; }
    std::vector<std::string> list(std::string_view prefix) const override {
        std::vector<std::string> out;
        for (auto const& [p,_]:files) if (p.starts_with(prefix)) out.push_back(p);
        return out;
    }
    bool contains(std::string_view path) const override { return files.contains(std::string(path)); }
    std::optional<std::string> read(std::string_view path) const override {
        auto it=files.find(std::string(path));
        return it==files.end() || path==unreadable ? std::nullopt : std::optional(it->second);
    }
};
auto source(std::string name,std::map<std::string,std::string> files) { return std::make_unique<OverrideSource>(std::move(name),std::move(files)); }
auto load(std::vector<std::unique_ptr<pack::PackSource>> sources) {
    pack::GunPackRegistry out; out.load(std::move(sources),[](auto const&){},[](auto const&){}); return out;
}
}
TEST_CASE("Resource operations preserve delegate order rather than guessing priority from extension") {
    std::string path="assets/fixture/probe/value.json";
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(source("z-first",{{path,"A"},{"assets/fixture/probe/only.json","only"}}));
    sources.push_back(source("a-second",{{path,"B"}}));
    sources.push_back(source("m-third",{{path,"C"}}));
    auto packs=load(std::move(sources));
    CHECK(packs.packs().at(0).name=="z-first");
    CHECK(packs.readFile(path,pack::ResourceLookup::Direct)=="A");
    CHECK(packs.readFile(path,pack::ResourceLookup::Listed)=="C");
    auto listed=packs.resolve(path,pack::ResourceLookup::ListedStack);
    auto stack=packs.resolve(path,pack::ResourceLookup::ResourceStack);
    REQUIRE(listed.size()==3); REQUIRE(stack.size()==3);
    for (size_t i=0;i<3;++i) CHECK(listed[i].bytes==stack[2-i].bytes);
    CHECK(listed[1].pack=="a-second");
    CHECK(packs.readFile("assets/fixture/probe/only.json",pack::ResourceLookup::Direct)=="only");
    CHECK(packs.resolve("assets/fixture/probe/missing.json",pack::ResourceLookup::ListedStack).empty());
    CHECK_FALSE(packs.readFile(path,pack::ResourceLookup::ListedStack));
}
TEST_CASE("An existing namespace does not hide later paths and a failed selected supplier does not fall back") {
    std::string path="assets/fixture/probe/value.bin";
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(source("first",{{"assets/fixture/probe/other.bin","other"}}));
    sources.push_back(source("second",{{path,"content"}}));
    auto broken=source("third",{{path,"unreadable"}}); broken->unreadable=path; sources.push_back(std::move(broken));
    auto packs=load(std::move(sources));
    CHECK(packs.readFile(path,pack::ResourceLookup::Direct)=="content");
    CHECK_FALSE(packs.readFile(path,pack::ResourceLookup::Listed));
    auto selected=packs.resolve(path,pack::ResourceLookup::Listed);
    REQUIRE(selected.size()==1); CHECK(selected.front().pack=="third"); CHECK_FALSE(selected.front().bytes);
}
TEST_CASE("Builtin resources stay beneath packs in direct queries and both kinds of stacks") {
    std::string path="assets/tacz/scripts/default_state_machine.lua";
    std::vector<std::unique_ptr<pack::PackSource>> sources;
    sources.push_back(source("one",{{path,"A"}})); sources.push_back(source("two",{{path,"B"}}));
    auto packs=load(std::move(sources));
    CHECK(packs.readFile(path,pack::ResourceLookup::Direct)=="A");
    CHECK(packs.readFile(path,pack::ResourceLookup::Listed)=="B");
    for (auto mode:{pack::ResourceLookup::ListedStack,pack::ResourceLookup::ResourceStack}) {
        auto stack=packs.resolve(path,mode); REQUIRE(stack.size()==3); CHECK(stack.front().builtin);
    }
    packs.clear(); CHECK(packs.readFile(path,pack::ResourceLookup::Direct).has_value());
}
TEST_CASE("Folder discovery preserves native enumeration, not a portable sorted-name assumption") {
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/("tacz-order-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path path; ~Cleanup(){std::error_code ec;fs::remove_all(path,ec);} } cleanup{root};
    fs::create_directories(root);
    for (auto name:{"zeta","Alpha","[middle]"}) {
        fs::create_directory(root/name); std::ofstream(root/name/"gunpack.meta.json")<<R"({"namespace":"fixture"})";
    }
    std::vector<std::string> expected;
    for (auto const& entry:fs::directory_iterator(root)) expected.push_back(entry.path().filename().string());
    pack::GunPackRegistry packs; packs.loadFolder(root,[](auto const&){},[](auto const&){});
    REQUIRE(packs.packs().size()==expected.size());
    for (size_t i=0;i<expected.size();++i) CHECK(packs.packs()[i].name==expected[i]);
}
