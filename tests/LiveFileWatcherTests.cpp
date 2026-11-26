#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "engine/core/LiveFileWatcher.h"

int main() {
    struct Fixture {
        std::filesystem::path root = std::filesystem::temp_directory_path() / ("tge-live-watch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } fixture;
    try {
        std::filesystem::create_directory(fixture.root);
        const auto path = fixture.root / "module.luau";
        const auto write = [&](const char* text) { std::ofstream stream(path, std::ios::binary); stream << text; };
        write("old");
        engine::core::JobSystem jobs(true, 2);
        engine::core::LiveFileWatcher watcher;
        watcher.initialize(&jobs); watcher.watch(path);
        auto now = engine::core::LiveFileWatcher::Clock::now();
        const auto poll = [&] { now += std::chrono::milliseconds(160); auto changes = watcher.poll(now); jobs.waitAll(); return changes; };
        write("partly written"); poll();
        write("new");
        if (!poll().empty() || !poll().empty()) throw std::runtime_error("Partial save escaped debounce");
        const auto changed = poll();
        if (changed.size()!=1 || changed.front().contents!="new") throw std::runtime_error("Stable save missing");
        for (int i=0;i<3;++i) if (!poll().empty()) throw std::runtime_error("Unchanged file reloaded repeatedly");
        const auto time = std::filesystem::last_write_time(path);
        write("NEW"); std::filesystem::last_write_time(path,time);
        poll(); poll();
        if (poll().empty()) throw std::runtime_error("Equal timestamp/size edit missed");
        watcher.clear();
        watcher.initialize(&jobs); watcher.watch(path); watcher.poll(now + std::chrono::seconds(1)); watcher.clear();
        write("size-bound seed");
        watcher.initialize(&jobs); watcher.watch(path);
        const auto writeBytes = [&](const std::string& bytes) {
            std::ofstream stream(path, std::ios::binary);
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!stream) throw std::runtime_error("Cannot write bounded watcher fixture");
        };
        constexpr std::size_t limit = 8U * 1024U * 1024U;
        std::string boundary(limit, 'x');
        boundary[65535] = '\0';
        boundary.back() = 'z';
        writeBytes(boundary);
        poll(); poll();
        const auto accepted = poll();
        if (accepted.size() != 1 || !accepted.front().error.empty() || accepted.front().contents != boundary)
            throw std::runtime_error("An exact-limit binary live file was rejected or truncated");
        writeBytes(boundary + "x");
        poll(); poll();
        const auto oversized = poll();
        if (oversized.size() != 1 || !oversized.front().contents.empty() || oversized.front().error.find("8 MiB") == std::string::npos)
            throw std::runtime_error("An oversized live file was published without a bounded diagnostic");
        write("recovered after oversized save");
        poll(); poll();
        const auto recovered = poll();
        if (recovered.size() != 1 || !recovered.front().error.empty() || recovered.front().contents != "recovered after oversized save")
            throw std::runtime_error("Watcher did not recover after an oversized save");
        watcher.clear();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Live watcher: stable saves, content changes, bounded reads, recovery and shutdown passed\n";
}
