#include "engine/game/SceneNetworkServer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <exception>
#include <fstream>
#include <optional>
#include <ratio>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/AtomicFile.h"
#include "engine/core/JobSystem.h"
#include "engine/core/LabOptions.h"
#include "engine/core/Log.h"
#include "engine/core/PlaySession.h"
#include "engine/game/GameplayState.h"
#include "engine/game/StateStack.h"
#include "engine/network/NetworkSession.h"
#include "engine/network/SteamTransport.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace engine::game {
namespace {

using Clock = std::chrono::steady_clock;

class TickSleeper final {
public:
    TickSleeper() = default;
    TickSleeper(const TickSleeper&) = delete;
    TickSleeper& operator=(const TickSleeper&) = delete;

    ~TickSleeper() {
#if defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
        if (m_timer) CloseHandle(m_timer);
#endif
    }

    void waitUntil(Clock::time_point deadline) {
#if defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
        if (m_timer) {
            const auto remaining = deadline - Clock::now();
            if (remaining <= Clock::duration::zero()) return;
            using TimerUnits = std::chrono::duration<LONGLONG, std::ratio<1, 10000000>>;
            LARGE_INTEGER due{};
            due.QuadPart = -std::chrono::ceil<TimerUnits>(remaining).count();
            if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE) &&
                WaitForSingleObject(m_timer, INFINITE) == WAIT_OBJECT_0) return;
            CloseHandle(m_timer);
            m_timer = nullptr;
        }
#endif
        std::this_thread::sleep_until(deadline);
    }

private:
#if defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
    HANDLE m_timer{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE)};
#endif
};

std::atomic_flag stopRequested = ATOMIC_FLAG_INIT;

struct LogScope {
    LogScope() { core::Log::init(); }
    ~LogScope() { core::Log::shutdown(); }
};

int publishError(const core::PlayLaunch& launch, const std::string& message) {
    ENGINE_LOG_ERROR("Network Play server: {}", message);
    if (!launch.networkReadyPath.empty()) {
        std::string ignored;
        core::writeFileAtomically(launch.networkReadyPath.parent_path() / "error.txt", message + "\n", ignored);
    }
    return 1;
}

struct SteamInviteRequest {
    uint64_t sequence{};
    uint64_t friendId{};
};

std::optional<SteamInviteRequest> readSteamInviteRequest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::array<char, 44> bytes{};
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input.eof() || input.bad() || input.gcount() < 4) return {};
    const std::string_view line(bytes.data(), static_cast<size_t>(input.gcount()) - 1U);
    if (bytes[line.size()] != '\n') return {};

    SteamInviteRequest request;
    const auto space = line.find(' ');
    if (space == line.npos || space == 0 || space + 1U == line.size()) return {};
    const auto* mid = line.data() + space;
    const auto* end = line.data() + line.size();
    const auto [sequenceEnd, sequenceError] = std::from_chars(line.data(), mid, request.sequence);
    const auto [friendEnd, friendError] = std::from_chars(mid + 1, end, request.friendId);
    if (sequenceEnd != mid || sequenceError != std::errc{} || request.sequence == 0 ||
        friendEnd != end || friendError != std::errc{}) return {};
    return request;
}

std::vector<std::uint64_t> readSteamAvatarRequests(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::array<char, net::MaxSteamAvatarRequests * 22 + 8> bytes{};
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())).eof() || input.bad()) return {};
    const auto data = nlohmann::json::parse(bytes.data(), bytes.data() + input.gcount(), nullptr, false);
    if (!data.is_array() || data.size() > net::MaxSteamAvatarRequests) return {};

    std::vector<std::uint64_t> requests;
    requests.reserve(data.size());
    for (const auto& entry : data) {
        if (!entry.is_number_unsigned()) return {};
        const auto id = entry.get<std::uint64_t>();
        if (!net::validSteamUserId(id)) return {};
        if (std::ranges::find(requests, id) == requests.end()) requests.push_back(id);
    }
    return requests;
}

class SteamInviteMailbox final {
public:
    explicit SteamInviteMailbox(const std::filesystem::path& directory)
        : m_request(directory / "steam-invite-request"), m_reply(directory / "steam-invite-result"),
          m_friendList(directory / net::SteamFriendListFile), m_avatarRequestsFile(directory / net::SteamAvatarRequestFile) {}

    void poll(net::SteamInvites& steam, net::SteamJoin join) {
        const auto now = Clock::now();
        if (now < m_nextPoll) return;
        m_nextPoll = now + std::chrono::milliseconds(100);
        pollFriends(steam, now);
        pollAvatars(steam);

        if (m_result.empty()) {
            const auto request = readSteamInviteRequest(m_request);
            if (!request || request->sequence <= m_lastSequence) return;
            m_lastSequence = request->sequence;
            std::string error;
            m_result = std::to_string(m_lastSequence) + "\n";
            const bool sent = steam.lobby() ? steam.inviteLobbyFriend(request->friendId, error)
                                            : steam.invite(request->friendId, join, error);
            if (!sent) m_result += error + "\n";
        }
        std::string error;
        if (core::writeFileAtomically(m_reply, m_result, error)) m_result.clear();
    }

private:
    void pollFriends(net::SteamInvites& steam, Clock::time_point now) {
        if (now < m_nextFriendRefresh) return;
        m_nextFriendRefresh = now + std::chrono::seconds(2);

        auto friends = steam.friends();
        if (m_friendsPublished && friends == m_publishedFriends) return;

        nlohmann::json data;
        if (friends) {
            data = nlohmann::json::array();
            for (const auto& entry : *friends) {
                data.push_back({{"id", entry.id}, {"name", entry.name}, {"presence", static_cast<unsigned>(entry.presence)}});
            }
        }
        std::string error;
        if (core::writeFileAtomically(m_friendList, data.dump(), error)) {
            m_friendsPublished = true;
            m_publishedFriends = std::move(friends);
        }
    }

    void pollAvatars(net::SteamInvites& steam) {
        auto requests = readSteamAvatarRequests(m_avatarRequestsFile);
        if (requests != m_avatarRequests) {
            m_avatarRequests = std::move(requests);
            m_nextAvatar = 0;
        }
        const auto budget = std::min<std::size_t>(6, m_avatarRequests.size());
        for (std::size_t i = 0; i < budget; ++i) {
            const auto id = m_avatarRequests[m_nextAvatar];
            m_nextAvatar = (m_nextAvatar + 1) % m_avatarRequests.size();
            const auto image = steam.smallAvatarHandle(id);
            if (!image || *image < 0) continue;
            const auto previous = m_avatarHandles.find(id);
            if (previous != m_avatarHandles.end() && previous->second == *image) continue;
            if (previous == m_avatarHandles.end() && (*image == 0 || m_avatarHandles.size() >= net::MaxSteamFriends)) continue;

            const auto file = m_avatarRequestsFile.parent_path() / net::steamAvatarFileName(id);
            if (*image == 0) {
                std::error_code removeError;
                std::filesystem::remove(file, removeError);
                if (!removeError) m_avatarHandles.erase(previous);
                continue;
            }
            const auto tga = steam.smallAvatarTga(*image);
            std::string error;
            if (!tga.empty() && core::writeFileAtomically(file, tga, error)) m_avatarHandles.insert_or_assign(id, *image);
        }
    }

    std::filesystem::path m_request;
    std::filesystem::path m_reply;
    std::filesystem::path m_friendList;
    std::filesystem::path m_avatarRequestsFile;
    Clock::time_point m_nextPoll{};
    Clock::time_point m_nextFriendRefresh{};
    bool m_friendsPublished{};
    std::optional<std::vector<net::SteamFriend>> m_publishedFriends;
    std::vector<std::uint64_t> m_avatarRequests;
    std::size_t m_nextAvatar{};
    std::unordered_map<std::uint64_t, int> m_avatarHandles;
    uint64_t m_lastSequence{};
    std::string m_result;
};

}

int runNetworkSceneServer(const core::PlayLaunch& options, net::SteamInvites* steam) {
    LogScope log;
    try {
        auto launch = options;
        std::string error;
        std::optional<net::SteamInvites> childSteam;
        if (launch.networkSteam && !steam) {
            childSteam.emplace();
            if (!childSteam->start(error)) return publishError(launch, "Steam initialization failed: " + error);
            steam = &*childSteam;
            launch.networkEndpoint = {0, net::SteamVirtualPort, steam->localUserId()};
        }
        launch.steamRuntime = steam;
        core::JobSystem jobs(true, 4U);
        StateStack stack;
        stack.setServices(&jobs, nullptr);
        stack.setStandalone(true);
        stack.setProject(&launch.project);
        stack.setSceneNetworkLaunch(&launch);
        auto scene = std::make_unique<GameplayState>(stack);
        const auto* gameplay = scene.get();
        stack.push(std::move(scene));
        stack.applyPendingChanges();
        if (!gameplay->networkSceneReady() || !gameplay->networkScenePort()) {
            const auto sceneError = gameplay->networkSceneError();
            stack.clear();
            stack.applyPendingChanges();
            return publishError(launch, sceneError.empty() ? "Could not start the scene server." : sceneError);
        }

        stopRequested.clear();
        const auto stop = [](int) { stopRequested.test_and_set(std::memory_order_relaxed); };
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
#ifdef _WIN32
        std::signal(SIGBREAK, stop);
#endif
        std::optional<SteamInviteMailbox> steamMailbox;
        if (steam && !launch.networkReadyPath.empty()) {
            const auto directory = launch.networkReadyPath.parent_path();
            if (!core::writeFileAtomically(directory / "steam-id",
                    std::to_string(launch.networkEndpoint.steamId) + "\n", error)) {
                stack.clear();
                stack.applyPendingChanges();
                return publishError(launch, error);
            }
            steamMailbox.emplace(directory);
        }
        if (!launch.networkReadyPath.empty() && !core::writeFileAtomically(launch.networkReadyPath,
                std::to_string(gameplay->networkScenePort()) + "\n", error)) {
            stack.clear();
            stack.applyPendingChanges();
            return publishError(launch, error);
        }
        auto endpoint = launch.networkEndpoint;
        endpoint.port = gameplay->networkScenePort();
        if (steam) {
            ENGINE_LOG_INFO("Network scene server listening on SteamID {} (virtual port {})", endpoint.steamId, endpoint.port);
            if (launch.networkSteamInvite && !steam->invite(launch.networkSteamInvite,
                    {endpoint.steamId, launch.networkCompatibility}, error)) {
                ENGINE_LOG_WARN("Could not send Steam invitation: {}", error);
            }
        } else {
            ENGINE_LOG_INFO("Network scene server listening on {} (UDP)", net::formatEndpoint(endpoint));
        }
        constexpr auto dt = 1.0 / net::TickRate;
        const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(dt));
        TickSleeper sleeper;
        auto next = Clock::now();
        auto nextStopCheck = next;
        const bool diagnostics = !core::LabOptions::environment("TGE_FRAME_DIAGNOSTICS").empty();
        auto sampleStart = next;
        Clock::duration totalWork{}, maxWork{}, maxStartLate{};
        uint64_t ticks = 0, skippedDeadlines = 0;
        while (!stopRequested.test(std::memory_order_relaxed)) {
            const auto started = Clock::now();
            if (!launch.networkStopPath.empty() && started >= nextStopCheck) {
                nextStopCheck = started + std::chrono::milliseconds(50);
                std::error_code markerError;
                if (std::filesystem::exists(launch.networkStopPath, markerError) || markerError) break;
            }
            stack.update(dt);
            if (steamMailbox) steamMailbox->poll(*steam, {endpoint.steamId, launch.networkCompatibility});
            const auto now = Clock::now();
            const auto deadline = next + interval;
            next = deadline;
            if (deadline + interval * 3 < now) next = now + interval;
            if (diagnostics) {
                const auto work = now - started;
                ++ticks;
                totalWork += work;
                maxWork = std::max(maxWork, work);
                maxStartLate = std::max(maxStartLate, started - (deadline - interval));
                if (next != deadline) skippedDeadlines += static_cast<uint64_t>((now - deadline) / interval) + 1;
                if (now - sampleStart >= std::chrono::seconds(5)) {
                    using Milliseconds = std::chrono::duration<double, std::milli>;
                    ENGINE_LOG_INFO("Network scene ticks: hz={:.2f} avg_work_ms={:.3f} max_work_ms={:.3f} "
                        "max_start_late_ms={:.3f} skipped_deadlines={}",
                        static_cast<double>(ticks) / std::chrono::duration<double>(now - sampleStart).count(),
                        Milliseconds(totalWork).count() / static_cast<double>(ticks), Milliseconds(maxWork).count(),
                        Milliseconds(maxStartLate).count(), skippedDeadlines);
                    sampleStart = now;
                    totalWork = maxWork = maxStartLate = Clock::duration::zero();
                    ticks = skippedDeadlines = 0;
                }
            }
            sleeper.waitUntil(next);
        }
        stack.clear();
        stack.applyPendingChanges();
        ENGINE_LOG_INFO("Network Play server stopped.");
        return 0;
    } catch (const std::exception& failure) {
        return publishError(options, failure.what());
    }
}

}
