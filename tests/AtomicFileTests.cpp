#include "engine/core/AtomicFile.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
namespace fs = std::filesystem;
using engine::core::writeFileAtomically;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture final {
    fs::path root;
    Fixture() {
        root = fs::temp_directory_path() / ("tge-atomic-file-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(fs::create_directory(root), "Could not exclusively create fixture");
    }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
    void requireNoTemporaryFiles() const {
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            require(!entry.path().filename().string().starts_with(".tge-save-"), "Atomic write leaked its temporary file");
        }
    }
};

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "Could not read test file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    require(static_cast<bool>(output), "Could not write test file");
}

void hardLinkTargetIsIsolated() {
    Fixture fixture;
    fs::create_directory(fixture.root / "ProjectA");
    fs::create_directory(fixture.root / "ProjectB");
    const auto other = fixture.root / "ProjectB" / "scene.json";
    const auto target = fixture.root / "ProjectA" / "scene.json";
    write(other, "other project scene");
    fs::create_hard_link(other, target);
    require(fs::equivalent(other, target), "Fixture must start with a shared hardlink");
    std::string error;
    require(writeFileAtomically(target, "saved active project scene", error), "Atomic replacement of hardlink failed");
    require(error.empty() && read(target) == "saved active project scene", "Target contents were not replaced");
    require(read(other) == "other project scene" && !fs::equivalent(other, target), "Saving active project changed another project's hardlink");
    fixture.requireNoTemporaryFiles();
}

void existingSidecarRemainsUntouched() {
    Fixture fixture;
    const auto target = fixture.root / "script.luau";
    const auto other = fixture.root / "outside.luau";
    auto sidecar = target;
    sidecar += ".tge-save";
    write(target, "original script");
    write(other, "other script");
    fs::create_hard_link(other, sidecar);
    std::string error;
    require(writeFileAtomically(target, "saved script", error), "Existing sidecar blocked save");
    require(read(target) == "saved script" && read(other) == "other script" && read(sidecar) == "other script",
            "Atomic save changed an existing sidecar or its linked file");
    require(fs::equivalent(sidecar, other), "Atomic save removed or replaced another sidecar");

    const auto linkedTarget = fixture.root / "linked.luau";
    std::error_code symlinkError;
    fs::create_symlink(other, linkedTarget, symlinkError);
    if (!symlinkError) {
        require(writeFileAtomically(linkedTarget, "local script", error), "Atomic save could not replace target symlink");
        require(!fs::is_symlink(linkedTarget) && read(linkedTarget) == "local script" && read(other) == "other script",
                "Atomic save followed target symlink outside its target");
    }
    fixture.requireNoTemporaryFiles();
}

void failedReplacementPreservesOriginal() {
    Fixture fixture;
    const auto directoryTarget = fixture.root / "original";
    fs::create_directory(directoryTarget);
    write(directoryTarget / "sentinel", "preserved");
    std::string error;
    require(!writeFileAtomically(directoryTarget, "replacement", error) && !error.empty(), "Replacing a directory must fail visibly");
    require(fs::is_directory(directoryTarget) && read(directoryTarget / "sentinel") == "preserved", "Failed replacement changed the original target");
    fixture.requireNoTemporaryFiles();
#if defined(_WIN32)
    const auto target = fixture.root / "locked.json";
    write(target, "original scene");
    const HANDLE file = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Could not lock original target for replacement failure");
    struct Lock final { HANDLE file; ~Lock() { CloseHandle(file); } } lock{file};
    require(!writeFileAtomically(target, "unsaved scene", error) && !error.empty(), "Locked file replacement should fail visibly");
    require(read(target) == "original scene", "Failed locked-file replacement truncated the original");
    fixture.requireNoTemporaryFiles();
#endif
}

void binaryAndEmptyWrites() {
    Fixture fixture;
    const auto target = fixture.root / fs::path(std::u8string(u8"\u0441\u0446\u0435\u043d\u0430.json"));
    std::string contents(200000U, 'a');
    contents[1] = '\0';
    contents[65535] = '\n';
    std::string error;
    require(writeFileAtomically(target, contents, error) && read(target) == contents, "Multi-chunk binary or Unicode path write was corrupted");
    require(writeFileAtomically(target, "", error) && read(target).empty(), "Empty file write failed");
    require(!writeFileAtomically(fixture.root / "missing" / "scene.json", "data", error) && !error.empty(), "Missing parent must fail visibly");
    fixture.requireNoTemporaryFiles();
}
} // namespace

int main() {
    try {
        hardLinkTargetIsIsolated();
        existingSidecarRemainsUntouched();
        failedReplacementPreservesOriginal();
        binaryAndEmptyWrites();
        std::cout << "AtomicFile tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
