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
        // Content comparison also catches equal-size writes with preserved timestamps.
        const auto time = std::filesystem::last_write_time(path);
        write("NEW"); std::filesystem::last_write_time(path,time);
        poll(); poll();
        if (poll().empty()) throw std::runtime_error("Equal timestamp/size edit missed");
        // clear/destruction must join an outstanding read before releasing its scheduler.
        watcher.clear();
        watcher.initialize(&jobs); watcher.watch(path); watcher.poll(now + std::chrono::seconds(1)); watcher.clear();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Live watcher: stable saves, content changes, deduplication and shutdown passed\n";
}
