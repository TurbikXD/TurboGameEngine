#include "engine/editor/ScriptFormatter.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using engine::editor::ScriptFormatter;
void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
ScriptFormatter::Result waitForResult(ScriptFormatter& formatter) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (auto result = formatter.poll()) return std::move(*result);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("formatter result timeout");
}
} // namespace

int main(const int argc, char** argv) {
#if !defined(_WIN32)
    (void)argc; (void)argv;
    std::cout << "Native formatter integration requires Windows.\n";
    return 0;
#else
    try {
        ScriptFormatter formatter;
        if (argc == 2) formatter.setExecutable(argv[1]);
        require(formatter.available(), "pinned StyLua runtime is missing");
        const std::string source =
            "-- keep this comment\n"
            "export type Point={x:number,y:number}\n"
            "local function add(x:number,y:number):number return x+y end\n"
            "local name = `point {add(1,2)}`\n"
            "return {add=add,name=name}\n";
        require(formatter.start("scripts/Typed.luau", source), "could not start formatting");
        auto formatted = waitForResult(formatter);
        require(formatted.success, formatted.error.c_str());
        require(formatted.documentPath == "scripts/Typed.luau" && formatted.original == source, "result must identify its exact source snapshot");
        require(formatted.formatted.find("-- keep this comment") != std::string::npos, "comments must survive formatting");
        require(formatted.formatted.find("export type Point") != std::string::npos && formatted.formatted.find("x: number") != std::string::npos,
                "Luau type declarations must survive formatting");
        require(formatted.formatted.find("    return x + y") != std::string::npos, "body should use four-space indentation");
        require(formatted.formatted.find("`point ") != std::string::npos, "Luau interpolation must survive formatting");
        require(formatter.start("scripts/Typed.luau", formatted.formatted), "could not reformat");
        const auto second = waitForResult(formatter);
        require(second.success && second.formatted == formatted.formatted, "formatter should be idempotent");
        const std::string invalid = "local function broken(\n";
        require(formatter.start("scripts/Broken.luau", invalid), "could not start syntax failure check");
        const auto rejected = waitForResult(formatter);
        require(!rejected.success && !rejected.error.empty() && rejected.original == invalid && rejected.formatted.empty(),
                "syntax errors must preserve the original document and provide diagnostics");
        require(!formatter.start("scripts/Large.luau", std::string(2U * 1024U * 1024U + 1U, ' ')), "oversized documents must be rejected");
        require(formatter.start("scripts/Cancel.luau", source), "could not start cancel check");
        formatter.cancel();
        const auto cancelled = waitForResult(formatter);
        require(!cancelled.success && cancelled.error.find("cancel") != std::string::npos, "cancellation should return without modifying source");
        std::cout << "StyLua integration tests passed: Luau types/comments/interpolation, idempotence, syntax errors, bounds, cancellation.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#endif
}
