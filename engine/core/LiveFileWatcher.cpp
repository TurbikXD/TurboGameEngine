#include "engine/core/LiveFileWatcher.h"

#include <array>
#include <fstream>
#include <map>
#include <stdexcept>
#include <utility>

namespace engine::core {
namespace {
LiveFileWatcher::Change read(const std::filesystem::path& path) {
    LiveFileWatcher::Change result{path, {}, {}};
    try {
        constexpr std::size_t limit = 8U * 1024U * 1024U;
        if (std::filesystem::file_size(path) > limit) throw std::runtime_error("Live file exceeds 8 MiB");
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot read live file");
        std::array<char, 65536> buffer;
        for (;;) {
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = static_cast<std::size_t>(stream.gcount());
            if (count > limit - result.contents.size()) throw std::runtime_error("Live file exceeds 8 MiB");
            result.contents.append(buffer.data(), count);
            if (stream.bad() || (stream.fail() && !stream.eof())) throw std::runtime_error("Live file read failed");
            if (stream.eof()) break;
        }
    } catch (const std::exception& error) { result.error = error.what(); result.contents.clear(); }
    return result;
}
bool same(const LiveFileWatcher::Change& a, const LiveFileWatcher::Change& b) {
    return a.contents == b.contents && a.error == b.error;
}
}
struct LiveFileWatcher::Impl {
    struct Entry { Change applied, pending; bool hasPending{}; };
    std::map<std::filesystem::path, Entry> files;
    JobSystem* jobs{};
    JobSystem::Handle job;
    std::shared_ptr<std::vector<Change>> reads;
    Clock::time_point next{};
};
LiveFileWatcher::LiveFileWatcher() : m_impl(std::make_unique<Impl>()) {}
LiveFileWatcher::~LiveFileWatcher() { clear(); }
void LiveFileWatcher::initialize(JobSystem* jobs) { clear(); m_impl->jobs = jobs; }
void LiveFileWatcher::clear() {
    auto& self = *m_impl;
    if (self.job.valid() && self.jobs) self.jobs->wait(self.job);
    self.job = {}; self.reads.reset(); self.files.clear(); self.next = {}; self.jobs = nullptr;
}
void LiveFileWatcher::watch(const std::filesystem::path& path) {
    auto& files = m_impl->files;
    if (!files.contains(path)) files.emplace(path, Impl::Entry{read(path), {}, false});
}
std::vector<LiveFileWatcher::Change> LiveFileWatcher::poll(const Clock::time_point now) {
    auto& self = *m_impl;
    std::vector<Change> changes;
    if (self.reads) {
        if (self.job.valid() && self.jobs && !self.jobs->isComplete(self.job)) return changes;
        if (self.job.valid() && self.jobs) self.jobs->wait(self.job);
        for (auto& value : *self.reads) {
            auto found = self.files.find(value.path);
            if (found == self.files.end()) continue;
            auto& entry = found->second;
            if (same(entry.applied, value)) entry.hasPending = false;
            else if (entry.hasPending && same(entry.pending, value)) {
                entry.applied = value; entry.hasPending = false; changes.push_back(std::move(value));
            } else { entry.pending = std::move(value); entry.hasPending = true; }
        }
        self.reads.reset(); self.job = {};
    }
    if (now < self.next || self.files.empty()) return changes;
    self.next = now + std::chrono::milliseconds(150);
    std::vector<std::filesystem::path> paths;
    for (const auto& [path, entry] : self.files) { (void)entry; paths.push_back(path); }
    self.reads = std::make_shared<std::vector<Change>>();
    const auto readFiles = [paths = std::move(paths), result = self.reads] {
        for (const auto& path : paths) result->push_back(read(path));
    };
    if (self.jobs) self.job = self.jobs->dispatch(readFiles, JobSystem::Priority::Low);
    else readFiles();
    return changes;
}
}
