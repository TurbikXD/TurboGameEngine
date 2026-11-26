#include "engine/core/PlaySession.h"
#include "engine/core/AtomicFile.h"
#include "engine/core/NetworkTestControl.h"
#include "engine/core/NetworkTestCsv.h"
#include "engine/network/NetworkSession.h"
#include "engine/network/SteamTransport.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <nlohmann/json.hpp>
#include <span>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

namespace engine::core {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Json = nlohmann::json;
constexpr const char* NetworkControlFolder = "control";
constexpr const char* SteamIdMarker = "steam-id";
constexpr const char* SteamInviteRequest = "steam-invite-request";
constexpr const char* SteamInviteResult = "steam-invite-result";

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

struct ProcessGroup {
#if defined(_WIN32)
    HANDLE job{};

    bool start(std::string& error) {
        job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return true;
        }
        error = "Could not create the Network Play process group, Windows error " + std::to_string(GetLastError());
        close();
        return false;
    }
    void close() {
        if (job) CloseHandle(job);
        job = nullptr;
    }
#else
    bool start(std::string&) { return true; }
    void close() {}
#endif
};

struct ChildProcess {
#if defined(_WIN32)
    HANDLE process{};
    DWORD processId{};
#else
    mutable pid_t process{-1};
    mutable int status{};
#endif

    bool start(const std::filesystem::path& binary, const std::filesystem::path& manifest,
               const ProcessGroup* group, std::string& error) {
        const auto directory = manifest.parent_path();
#if defined(_WIN32)
        std::wstring command = L"\"" + binary.wstring() + L"\" --play-session \"" + manifest.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        const DWORD flags = CREATE_NO_WINDOW | (group ? CREATE_SUSPENDED : 0);
        if (!CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, FALSE, flags,
                            nullptr, directory.c_str(), &startup, &info)) {
            error = "Could not launch " + utf8(directory.filename()) + ", Windows error " + std::to_string(GetLastError());
            return false;
        }
        process = info.hProcess;
        processId = info.dwProcessId;
        if (group && (!AssignProcessToJobObject(group->job, process) || ResumeThread(info.hThread) == DWORD(-1))) {
            error = "Could not start " + utf8(directory.filename()) + " in the Network Play process group, Windows error " +
                std::to_string(GetLastError());
            CloseHandle(info.hThread);
            terminate();
            close();
            return false;
        }
        CloseHandle(info.hThread);
#else
        const auto binaryName = binary.string();
        const auto manifestName = manifest.string();
        const auto directoryName = directory.string();
#if defined(__linux__)
        const auto parent = getpid();
#else
        (void)group;
#endif
        process = fork();
        if (process == 0) {
#if defined(__linux__)
            if (group && (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent)) _exit(125);
#endif
            if (chdir(directoryName.c_str()) == 0) {
                execl(binaryName.c_str(), binaryName.c_str(), "--play-session", manifestName.c_str(), static_cast<char*>(nullptr));
            }
            _exit(127);
        }
        if (process < 0) {
            error = "Could not launch " + directoryName;
            return false;
        }
#endif
        return true;
    }

    bool running() const {
#if defined(_WIN32)
        return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
#else
        if (process < 0) return false;
        const auto result = waitpid(process, &status, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR)) return true;
        process = -1;
        return false;
#endif
    }

    unsigned long exitCode() const {
#if defined(_WIN32)
        DWORD code = 1;
        if (process) GetExitCodeProcess(process, &code);
        return code;
#else
        return WIFEXITED(status) ? static_cast<unsigned long>(WEXITSTATUS(status))
                                 : static_cast<unsigned long>(128 + WTERMSIG(status));
#endif
    }

    void requestClose() const {
        if (!running()) return;
#if defined(_WIN32)
        EnumWindows([](HWND window, LPARAM value) -> BOOL {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner == static_cast<DWORD>(value)) PostMessageW(window, WM_CLOSE, 0, 0);
            return TRUE;
        }, static_cast<LPARAM>(processId));
#else
        kill(process, SIGTERM);
#endif
    }

    void requestTerminate() const {
        if (!running()) return;
#if defined(_WIN32)
        TerminateProcess(process, 0);
#else
        kill(process, SIGKILL);
#endif
    }

    void terminate() const {
        if (!running()) return;
        requestTerminate();
#if defined(_WIN32)
        WaitForSingleObject(process, 1000);
#else
        if (process >= 0) {
            while (waitpid(process, &status, 0) < 0 && errno == EINTR) {}
            process = -1;
        }
#endif
    }

    void close() {
#if defined(_WIN32)
        if (process) CloseHandle(process);
        process = nullptr;
        processId = 0;
#else
        process = -1;
        status = 0;
#endif
    }
};

bool waitForChildren(std::span<ChildProcess> children, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (std::ranges::any_of(children, &ChildProcess::running)) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

Json descriptor(const PlayLaunch& options) {
    return {{"version", 1}, {"name", options.project.name},
        {"root", utf8(std::filesystem::absolute(options.project.root))},
        {"assets", utf8(std::filesystem::absolute(options.project.assetRoot))},
        {"liveScene", options.project.scenePath.empty() ? std::string{} : utf8(std::filesystem::absolute(options.project.scenePath))},
        {"networkPlayerLimit", options.project.networkPlayerLimit},
        {"width", options.width}, {"height", options.height}, {"vsync", options.vsync}, {"device", options.device}};
}

std::optional<std::uint64_t> parseUnsignedLine(std::string_view text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    if (text.empty()) return {};
    std::uint64_t number{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
    return number;
}

std::optional<std::uint64_t> readUnsignedLine(const std::filesystem::path& marker) {
    std::ifstream input(marker, std::ios::binary);
    std::array<char, 24> bytes{};
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input.eof() || input.bad()) return {};
    return parseUnsignedLine({bytes.data(), static_cast<std::size_t>(input.gcount())});
}

std::optional<std::uint16_t> readPort(const std::filesystem::path& marker) {
    const auto port = readUnsignedLine(marker);
    if (!port || *port == 0 || *port > 65535) return {};
    return static_cast<std::uint16_t>(*port);
}

std::optional<std::string> readInviteResult(const std::filesystem::path& marker, std::uint64_t sequence) {
    std::ifstream input(marker, std::ios::binary);
    std::array<char, 1024> bytes{};
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input.eof() || input.bad()) return {};
    std::string_view text(bytes.data(), static_cast<std::size_t>(input.gcount()));
    const auto newline = text.find('\n');
    if (newline == std::string_view::npos || parseUnsignedLine(text.substr(0, newline)) != sequence) return {};
    text.remove_prefix(newline + 1);
    return std::string(text.substr(0, text.find_first_of("\r\n")));
}

bool prepareSteamAppId(const std::filesystem::path& binary, const std::filesystem::path& directory, std::string& error) {
    const auto path = binary.parent_path() / "steam_appid.txt";
    if (!std::filesystem::exists(path)) return true;
    const auto appId = readUnsignedLine(path);
    if (!appId || *appId == 0 || *appId > std::numeric_limits<std::uint32_t>::max()) {
        error = "Invalid Steam App ID in the game's steam_appid.txt.";
        return false;
    }
    return writeFileAtomically(directory / path.filename(), std::to_string(*appId) + "\n", error);
}

bool validOnlineLaunch(const PlayLaunch& launch) {
    if (launch.networkRole == SceneNetworkRole::Offline || launch.networkMaxPeers == 0 ||
        launch.networkMaxPeers > net::MaxPeers || !launch.networkCompatibility) return false;
    const auto& endpoint = launch.networkEndpoint;
    if (launch.networkSteam) {
        return launch.networkHost.empty() && endpoint.address == 0 && endpoint.port == net::SteamVirtualPort;
    }
    if (endpoint.steamId) return false;
    if (!launch.networkHost.empty()) {
        return launch.networkRole == SceneNetworkRole::Client && launch.networkHost.size() <= 259 &&
            launch.networkHost.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-:") ==
                std::string::npos;
    }
    const auto firstOctet = endpoint.address >> 24;
    return endpoint.port && ((launch.networkRole == SceneNetworkRole::Server && endpoint.address == 0) ||
        (firstOctet > 0 && firstOctet < 224));
}

std::optional<std::uint64_t> readUnsignedField(const Json& object, const char* key) {
    const auto value = object.find(key);
    if (value == object.end() || !value->is_number_unsigned()) return {};
    return value->get<std::uint64_t>();
}

bool readSteamFriends(const std::filesystem::path& path,
                      std::shared_ptr<const std::vector<net::SteamFriend>>& snapshot) {
    constexpr auto MaxBytes = net::MaxSteamFriends * (6 * net::MaxSteamFriendNameBytes + 64) + 16;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (size <= 0 || static_cast<std::streamoff>(size) > static_cast<std::streamoff>(MaxBytes)) return false;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
        input.peek() != std::char_traits<char>::eof() || input.bad()) return false;

    const auto data = Json::parse(bytes, nullptr, false);
    if (data.is_null()) {
        snapshot.reset();
        return true;
    }
    if (!data.is_array() || data.size() > net::MaxSteamFriends) return false;

    std::vector<net::SteamFriend> friends;
    friends.reserve(data.size());
    std::unordered_set<std::uint64_t> ids;
    ids.reserve(data.size());
    for (const auto& entry : data) {
        const auto id = readUnsignedField(entry, "id");
        const auto name = entry.find("name");
        const auto online = entry.find("online");
        const auto presence = entry.find("presence");
        if (!id || !net::validSteamUserId(*id) || !ids.insert(*id).second ||
            name == entry.end() || !name->is_string() || (online != entry.end() && !online->is_boolean())) return false;
        const auto& text = name->get_ref<const std::string&>();
        if (text.empty() || text.size() > net::MaxSteamFriendNameBytes ||
            std::ranges::any_of(text, [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; })) return false;
        auto state = online != entry.end() && online->get<bool>() ? net::SteamFriendPresence::Online
                                                               : net::SteamFriendPresence::Offline;
        if (presence != entry.end()) {
            const auto value = readUnsignedField(entry, "presence");
            if (!value || *value > static_cast<std::uint64_t>(net::SteamFriendPresence::Busy)) return false;
            state = static_cast<net::SteamFriendPresence>(*value);
        }
        friends.push_back({*id, text, state});
    }
    snapshot = std::make_shared<const std::vector<net::SteamFriend>>(std::move(friends));
    return true;
}

std::optional<int> readBoundedInteger(const Json& object, const char* key, int minimum, int maximum,
                                      std::optional<int> fallback = {}) {
    const auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (!it->is_number_integer() ||
        (it->is_number_unsigned() && it->get<std::uint64_t>() > static_cast<std::uint64_t>(maximum))) return {};
    const auto value = it->get<std::int64_t>();
    if (value < minimum || value > maximum) return {};
    return static_cast<int>(value);
}

std::string invalidNetworkOptions() {
    return "Network Play requires 1-" + std::to_string(net::MaxNetworkTestClients) + " clients, 0-" +
        std::to_string(net::MaxNetworkTestLatencyMs) + " ms latency and 0-" +
        std::to_string(net::MaxNetworkTestLossPercent) + "% packet loss.";
}

std::string childFailure(const std::filesystem::path& directory, unsigned long code) {
    std::string message = "Network Play " + utf8(directory.filename()) + " exited with code " + std::to_string(code) + ".";
    {
        std::ifstream marker(directory.parent_path() / "error.txt", std::ios::binary);
        std::array<char, 512> detail{};
        marker.read(detail.data(), static_cast<std::streamsize>(detail.size()));
        const std::string_view explanation(detail.data(), static_cast<std::size_t>(marker.gcount()));
        if (!explanation.empty()) {
            message += " ";
            message += explanation.substr(0, explanation.find_first_of("\r\n"));
            return message;
        }
    }
    for (const auto* name : {"crash-last.txt", "engine.log"}) {
        std::ifstream input(directory / "logs" / name, std::ios::binary | std::ios::ate);
        const auto size = input.tellg();
        if (size <= 0) continue;
        std::array<char, 4096> buffer{};
        const auto count = (std::min)(static_cast<std::streamoff>(size), static_cast<std::streamoff>(buffer.size()));
        input.seekg(-count, std::ios::end);
        input.read(buffer.data(), count);
        std::string_view tail(buffer.data(), static_cast<std::size_t>(input.gcount()));
        const bool engineLog = name == std::string_view("engine.log");
        auto detail = tail.rfind(engineLog ? "[error]" : "Detail:");
        if (engineLog && detail != std::string_view::npos && detail > 0 &&
            tail.substr(detail).find("App initialization failed") != std::string_view::npos) {
            const auto prior = tail.rfind("[error]", detail - 1);
            if (prior != std::string_view::npos) detail = prior;
        }
        if (detail != std::string_view::npos) {
            tail.remove_prefix(detail);
            message += " ";
            message += tail.substr(0, (std::min)(tail.find_first_of("\r\n"), std::size_t{220}));
            break;
        }
    }
    return message;
}

constexpr std::uint64_t TrafficWindowMs = 2000;
constexpr std::uint64_t ProbeWindowMs = 5000;
constexpr auto TelemetryFreshness = 1500ms;
constexpr std::size_t MaxTelemetrySamples = 32;

bool lossCountersAdvance(const net::NetworkLossCounters& before, const net::NetworkLossCounters& after) {
    return after.packets >= before.packets && after.dropped >= before.dropped &&
        after.dropped - before.dropped <= after.packets - before.packets;
}

struct TelemetryHistory {
    using Sample = net::NetworkTestTelemetrySample;
    using LossField = net::NetworkLossCounters Sample::*;

    std::deque<Sample> samples;
    Clock::time_point lastUpdate;

    void add(const Sample& sample, Clock::time_point now) {
        if (!samples.empty()) {
            const auto& last = samples.back();
            if (sample.elapsedMs <= last.elapsedMs || sample.txPackets < last.txPackets ||
                sample.rxPackets < last.rxPackets || sample.txBytes < last.txBytes || sample.rxBytes < last.rxBytes ||
                !lossCountersAdvance(last.outbound, sample.outbound) || !lossCountersAdvance(last.probes, sample.probes)) {
                return;
            }
            if (sample.elapsedMs - last.elapsedMs > static_cast<std::uint64_t>(TelemetryFreshness.count())) {
                samples.clear();
            }
        }
        samples.push_back(sample);
        lastUpdate = now;
        while (samples.size() > MaxTelemetrySamples ||
               (samples.size() > 2 && sample.elapsedMs - samples[1].elapsedMs >= ProbeWindowMs)) {
            samples.pop_front();
        }
    }

    bool fresh(Clock::time_point now) const {
        return !samples.empty() && now - lastUpdate <= TelemetryFreshness;
    }

    const Sample* baseline(std::uint64_t windowMs) const {
        if (samples.size() < 2) return nullptr;
        const auto elapsed = samples.back().elapsedMs;
        const auto cutoff = elapsed > windowMs ? elapsed - windowMs : 0;
        const auto first = std::ranges::lower_bound(samples, cutoff, {}, &Sample::elapsedMs);
        return first != samples.begin() && first->elapsedMs > cutoff ? &*std::prev(first) : &*first;
    }

    std::optional<double> loss(std::uint64_t windowMs, LossField field) const {
        const auto* older = baseline(windowMs);
        if (!older) return {};
        const auto& previous = older->*field;
        const auto& current = samples.back().*field;
        const auto packets = current.packets - previous.packets;
        if (!packets) return {};
        return 100.0 * static_cast<double>(current.dropped - previous.dropped) / static_cast<double>(packets);
    }
};

struct ClientTelemetry {
    std::uint64_t nonce{};
    std::uint32_t revision{};
    TelemetryHistory client;
    TelemetryHistory server;
    net::NetworkTestMetrics metrics;
    std::optional<double> lastUplinkLoss;
    std::optional<double> lastDownlinkLoss;
    std::optional<double> lastProbeLoss;

    void reset(std::uint64_t nextNonce, std::uint32_t nextRevision) {
        *this = {};
        nonce = nextNonce;
        revision = nextRevision;
    }

    void setContext(const net::NetworkTestClientStatus& status, std::uint32_t desiredRevision) {
        const auto nextNonce = status.state == net::NetworkTestLinkState::TimedOut && !status.nonce ? nonce : status.nonce;
        if (nextNonce != nonce || desiredRevision != revision) reset(nextNonce, desiredRevision);
        if (status.state != net::NetworkTestLinkState::Connected) metrics.rttMs.reset();
    }

    net::NetworkTestMetrics sample(const std::optional<net::NetworkTestTelemetrySample>& clientSample,
                                  const std::optional<net::NetworkTestTelemetrySample>& serverSample,
                                  net::NetworkTestLinkState state, Clock::time_point now) {
        const auto matches = [this](const auto& candidate) {
            return nonce && candidate && candidate->nonce == nonce && candidate->revision == revision;
        };
        if (matches(clientSample)) client.add(*clientSample, now);
        if (matches(serverSample)) server.add(*serverSample, now);
        net::NetworkTestMetrics measured{};
        const bool clientFresh = client.fresh(now);
        const bool serverFresh = server.fresh(now);
        if (clientFresh) {
            const auto& latest = client.samples.back();
            if (state == net::NetworkTestLinkState::Connected) measured.rttMs = latest.rttMs;
            if (const auto* before = client.baseline(TrafficWindowMs)) {
                const auto scale = 1000.0 / static_cast<double>(latest.elapsedMs - before->elapsedMs);
                measured.upload = net::NetworkTestTrafficRate{
                    static_cast<double>(latest.txBytes - before->txBytes) * scale,
                    static_cast<double>(latest.txPackets - before->txPackets) * scale};
                measured.download = net::NetworkTestTrafficRate{
                    static_cast<double>(latest.rxBytes - before->rxBytes) * scale,
                    static_cast<double>(latest.rxPackets - before->rxPackets) * scale};
            }
            measured.uplinkLossPercent = client.loss(TrafficWindowMs, &net::NetworkTestTelemetrySample::outbound);
            measured.probeLossPercent = client.loss(ProbeWindowMs, &net::NetworkTestTelemetrySample::probes);
            if (measured.uplinkLossPercent) lastUplinkLoss = measured.uplinkLossPercent;
            if (measured.probeLossPercent) lastProbeLoss = measured.probeLossPercent;
        }
        if (serverFresh) {
            measured.downlinkLossPercent = server.loss(TrafficWindowMs, &net::NetworkTestTelemetrySample::outbound);
            if (measured.downlinkLossPercent) lastDownlinkLoss = measured.downlinkLossPercent;
        }
        metrics = measured;
        const bool timedOut = state == net::NetworkTestLinkState::TimedOut;
        if (clientFresh || timedOut) {
            metrics.uplinkLossPercent = lastUplinkLoss;
            metrics.probeLossPercent = lastProbeLoss;
        }
        if (serverFresh || timedOut) metrics.downlinkLossPercent = lastDownlinkLoss;
        if (state != net::NetworkTestLinkState::Connected) {
            measured.uplinkLossPercent.reset();
            measured.downlinkLossPercent.reset();
            measured.probeLossPercent.reset();
        }
        return measured;
    }
};

void writeHistoryCsvRow(std::ostream& csv, std::size_t clientIndex, const net::NetworkTestHistoryPoint& point) {
    const auto& profile = point.profile;
    const auto& metrics = point.metrics;
    csv << clientIndex << ',' << point.elapsedMs << ',' << point.breakBefore << ',' << profile.revision << ','
        << profile.conditions.latencyMs << ',' << profile.conditions.lossPercent;
    const auto field = [&csv](const auto& value) {
        csv << ',';
        if (value) csv << *value;
    };
    const auto traffic = [&csv](const auto& rate) {
        csv << ',';
        if (rate) csv << rate->bytesPerSecond;
        csv << ',';
        if (rate) csv << rate->packetsPerSecond;
    };
    field(metrics.rttMs);
    field(metrics.uplinkLossPercent);
    field(metrics.downlinkLossPercent);
    field(metrics.probeLossPercent);
    traffic(metrics.upload);
    traffic(metrics.download);
    csv << "\r\n";
}
}

struct PlaySession::Impl {
    struct ClientControl {
        net::NetworkTestLinkState state{net::NetworkTestLinkState::Starting};
        std::uint32_t confirmedRevision{};
        ClientTelemetry telemetry;
        net::NetworkTestHistory history;
        bool historyBreak{true};
    };

    std::filesystem::path directory;
    std::filesystem::path binary;
    std::array<ChildProcess, net::MaxNetworkTestClients + 1> children;
    ProcessGroup group;
    bool networkActive{};
    std::uint16_t networkPort{};
    std::size_t nextClient{1};
    Clock::time_point networkStarted;
    std::uint64_t networkStoppedElapsedMs{};
    Clock::time_point nextPoll;
    Clock::time_point nextTelemetryPoll;
    Clock::time_point deadline;
    net::NetworkTestOptions networkOptions{};
    std::vector<net::NetworkTestProfile> networkProfiles;
    std::vector<ClientControl> clientControls;
    Json networkDescriptor;
    std::string networkError;
    OnlinePlayStatus online;
    Clock::time_point nextSteamFriendsPoll{};
    std::optional<std::filesystem::file_time_type> steamFriendsWriteTime;
    struct AvatarFile {
        std::filesystem::path path;
        Clock::time_point nextCheck{};
    };
    std::unordered_map<std::uint64_t, AvatarFile> steamAvatarFiles;
    std::vector<std::uint64_t> steamAvatarRequests;
    std::vector<std::uint64_t> publishedAvatarRequests;
    Clock::time_point nextSteamAvatarPoll{};
    std::uint64_t inviteSequence{};
    Clock::time_point inviteDeadline;
    bool onlineSteam{};

    [[nodiscard]] std::uint64_t elapsedMs(Clock::time_point now) const {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - networkStarted).count());
    }

    [[nodiscard]] std::uint64_t historyElapsedMs() const {
        return networkActive ? elapsedMs(Clock::now()) : networkStoppedElapsedMs;
    }

    [[nodiscard]] std::uint64_t oldestHistoryElapsedMs() const {
        const auto elapsed = historyElapsedMs();
        return elapsed > net::NetworkTestHistoryWindowMs ? elapsed - net::NetworkTestHistoryWindowMs : 0;
    }

    bool startChild(std::size_t index, std::uint16_t port, std::string& error) {
        const auto child = directory / (index == 0 ? "server" : "client-" + std::to_string(index));
        if (!std::filesystem::create_directory(child)) {
            error = "Could not create a Network Play process directory.";
            return false;
        }
        const auto manifest = child / "play.json";
        auto details = networkDescriptor;
        details["network"] = {{"role", index == 0 ? "server" : "client"}, {"port", port}, {"client", index},
            {"clientCount", networkOptions.clientCount}, {"latencyMs", networkOptions.latencyMs},
            {"lossPercent", networkOptions.lossPercent}, {"liveControl", true}};
        return writeFileAtomically(manifest, details.dump(2), error) && children[index].start(binary, manifest, &group, error);
    }
};

PlaySession::PlaySession() : m_impl(std::make_unique<Impl>()) {}
PlaySession::~PlaySession() { stop(); }
const std::filesystem::path& PlaySession::directory() const { return m_impl->directory; }
const std::string& PlaySession::networkError() const { return m_impl->networkError; }
const OnlinePlayStatus& PlaySession::onlineStatus() const { return m_impl->online; }

std::vector<net::NetworkTestClientView> PlaySession::networkTestClients() const {
    const auto& impl = *m_impl;
    std::vector<net::NetworkTestClientView> clients;
    clients.reserve(impl.networkProfiles.size());
    for (std::size_t slot = 0; slot < impl.networkProfiles.size(); ++slot) {
        const auto& profile = impl.networkProfiles[slot];
        const auto& control = impl.clientControls[slot];
        const auto index = static_cast<unsigned>(slot + 1);
        const bool editable = impl.networkActive && index < impl.nextClient && control.state != net::NetworkTestLinkState::Closed;
        clients.push_back({index, profile.conditions, control.state, editable,
                           editable && control.confirmedRevision != profile.revision, control.telemetry.metrics});
    }
    return clients;
}

net::NetworkTestHistoryView PlaySession::networkTestHistory(unsigned index) const {
    const auto& impl = *m_impl;
    if (!index || index > impl.clientControls.size()) return {};
    return {impl.historyElapsedMs(), &impl.clientControls[index - 1].history};
}

bool PlaySession::hasNetworkTestHistory() const {
    const auto oldest = m_impl->oldestHistoryElapsedMs();
    return std::ranges::any_of(m_impl->clientControls, [oldest](const auto& control) {
        const auto& points = control.history.points();
        return !points.empty() && points.back().elapsedMs >= oldest;
    });
}

bool PlaySession::exportNetworkTestHistory(const std::filesystem::path& target, std::string& error) const {
    error.clear();
    try {
        const auto oldest = m_impl->oldestHistoryElapsedMs();
        std::ostringstream csv;
        csv.imbue(std::locale::classic());
        csv << std::setprecision(std::numeric_limits<double>::max_digits10)
            << NetworkTestCsvHeader << "\r\n";
        bool wroteRows = false;
        for (std::size_t slot = 0; slot < m_impl->clientControls.size(); ++slot) {
            const auto& points = m_impl->clientControls[slot].history.points();
            const auto first = std::ranges::lower_bound(points, oldest, {}, &net::NetworkTestHistoryPoint::elapsedMs);
            for (auto point = first; point != points.end(); ++point) {
                writeHistoryCsvRow(csv, slot + 1, *point);
                wroteRows = true;
            }
        }
        if (!wroteRows) { error = "No Network Test samples to export."; return false; }
        if (!csv) { error = "Could not format the Network Test CSV."; return false; }
        return writeFileAtomically(target, std::move(csv).str(), error);
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

void PlaySession::clearNetworkTestHistory() {
    auto& impl = *m_impl;
    if (impl.networkActive) return;
    impl.networkProfiles.clear();
    impl.clientControls.clear();
    impl.networkStoppedElapsedMs = 0;
}

bool PlaySession::setNetworkTestConditions(unsigned index, net::NetworkTestConditions conditions, std::string& error) {
    auto& impl = *m_impl;
    error.clear();
    if (!conditions.valid()) {
        error = "Latency or packet loss is outside the Network Test limits.";
        return false;
    }
    if (!impl.networkActive || !index || index >= impl.nextClient || !impl.children[index].running() ||
        impl.clientControls[index - 1].state == net::NetworkTestLinkState::Closed) {
        error = "This Network Test client is not running.";
        return false;
    }
    const auto& current = impl.networkProfiles[index - 1];
    if (current.conditions == conditions) return true;
    if (current.revision == std::numeric_limits<std::uint32_t>::max()) {
        error = "The Network Test profile revision limit was reached.";
        return false;
    }
    try {
        auto profiles = impl.networkProfiles;
        profiles[index - 1] = {conditions, current.revision + 1};
        if (!writeNetworkTestProfiles(impl.directory / NetworkControlFolder, profiles, error)) return false;
        impl.networkProfiles.swap(profiles);
        auto& control = impl.clientControls[index - 1];
        control.telemetry.reset(control.telemetry.nonce, impl.networkProfiles[index - 1].revision);
        control.historyBreak = true;
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

bool PlaySession::running() const {
    return m_impl->networkActive || m_impl->online.role != SceneNetworkRole::Offline || m_impl->children.front().running();
}

void PlaySession::stop() {
    auto& impl = *m_impl;
    const auto children = std::span(impl.children).first(impl.nextClient);
    if (impl.networkActive) {
        impl.networkStoppedElapsedMs = impl.elapsedMs(Clock::now());
        for (auto& control : impl.clientControls) control.history.prune(impl.networkStoppedElapsedMs);
        for (const auto& client : children.subspan(1)) client.requestClose();
    }
    if (impl.networkActive || impl.online.role == SceneNetworkRole::Server) {
        if (impl.children.front().running()) {
            try {
                std::string error;
                if (!writeFileAtomically(impl.directory / "stop", "\n", error)) impl.children.front().requestClose();
            } catch (...) {
                impl.children.front().requestClose();
            }
        }
    } else {
        impl.children.front().requestClose();
    }
    if (!waitForChildren(children, 2s)) {
        for (const auto& child : children) child.requestTerminate();
        if (!waitForChildren(children, 1s)) {
#if defined(_WIN32)
            impl.group.close();
#else
            for (const auto& child : children) child.terminate();
#endif
        }
    }
    for (auto& child : children) child.close();
    impl.group.close();
    impl.networkActive = false;
    impl.networkPort = 0;
    impl.nextClient = 1;
    impl.networkDescriptor.clear();
    impl.networkError.clear();
    impl.online = {};
    impl.steamFriendsWriteTime.reset();
    impl.nextSteamFriendsPoll = {};
    impl.steamAvatarFiles.clear();
    impl.steamAvatarRequests.clear();
    impl.publishedAvatarRequests.clear();
    impl.nextSteamAvatarPoll = {};
    impl.onlineSteam = false;
    impl.inviteSequence = 0;
    if (!impl.directory.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(impl.directory, ignored);
        impl.directory.clear();
    }
}

bool PlaySession::prepare(const std::filesystem::path& executable, const std::string& scene, std::string& error) {
    auto& impl = *m_impl;
    impl.binary = std::filesystem::absolute(executable);
    if (!std::filesystem::is_regular_file(impl.binary)) { error = "Game executable was not found."; return false; }
    const auto temp = std::filesystem::temp_directory_path();
    const auto stamp = std::to_string(Clock::now().time_since_epoch().count());
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = temp / ("tge-play-" + stamp + "-" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate)) { impl.directory = candidate; break; }
    }
    if (impl.directory.empty()) { error = "Could not create an isolated Play session."; return false; }
    return writeFileAtomically(impl.directory / "scene.json", scene, error);
}

bool PlaySession::start(const std::filesystem::path& executable, const std::string& scene,
                        const PlayLaunch& options, std::string& error) {
    error.clear();
    if (running()) { error = "A standalone game is already running."; return false; }
    stop();
    try {
        const bool prepared = prepare(executable, scene, error);
        const auto manifest = m_impl->directory / "play.json";
        if (!prepared || !writeFileAtomically(manifest, descriptor(options).dump(2), error) ||
            !m_impl->children.front().start(m_impl->binary, manifest, nullptr, error)) {
            stop(); return false;
        }
        return true;
    } catch (const std::exception& failure) { error = failure.what(); stop(); return false; }
}

bool PlaySession::startNetwork(const std::filesystem::path& executable, const std::string& scene,
                               const PlayLaunch& options, std::string& error) {
    error.clear();
    if (running()) { error = "A Network Play session is already running."; return false; }
    if (!options.networkOptions.valid()) { error = invalidNetworkOptions(); return false; }
    if (options.project.networkPlayerLimit &&
        static_cast<unsigned>(options.networkOptions.clientCount) > options.project.networkPlayerLimit) {
        error = "This project supports up to " + std::to_string(options.project.networkPlayerLimit) + " players.";
        return false;
    }
    stop();
    auto& impl = *m_impl;
    try {
        if (!prepare(executable, scene, error) || !impl.group.start(error)) { stop(); return false; }
        impl.networkDescriptor = descriptor(options);
        impl.networkDescriptor["liveScene"] = "";
        impl.networkOptions = options.networkOptions;
        const auto clientCount = static_cast<std::size_t>(options.networkOptions.clientCount);
        std::vector<net::NetworkTestProfile> profiles(clientCount, {options.networkOptions.conditions(), 1});
        std::vector<Impl::ClientControl> controls(clientCount);
        const auto controlDirectory = impl.directory / NetworkControlFolder;
        if (!std::filesystem::create_directory(controlDirectory)) {
            error = "Could not create the Network Test control directory.";
            stop();
            return false;
        }
        if (!writeNetworkTestProfiles(controlDirectory, profiles, error)) { stop(); return false; }
        impl.networkStarted = Clock::now();
        if (!impl.startChild(0, 0, error)) { stop(); return false; }
        impl.networkProfiles.swap(profiles);
        impl.clientControls.swap(controls);
        impl.networkActive = true;
        impl.nextPoll = Clock::now();
        impl.nextTelemetryPoll = impl.nextPoll;
        impl.deadline = impl.nextPoll + 30s;
        return true;
    } catch (const std::exception& failure) { error = failure.what(); stop(); return false; }
}

bool PlaySession::startOnline(const std::filesystem::path& executable, const std::string& scene,
                             PlayLaunch options, std::string& error) {
    error.clear();
    if (running()) { error = "An online game process is already running."; return false; }
    if (!validOnlineLaunch(options)) { error = "Invalid multiplayer role, endpoint or scene identity."; return false; }
    if (options.networkRole == SceneNetworkRole::Server && options.project.networkPlayerLimit &&
        options.networkMaxPeers > options.project.networkPlayerLimit) {
        error = "This project supports up to " + std::to_string(options.project.networkPlayerLimit) + " players.";
        return false;
    }
    stop();
    auto& impl = *m_impl;
    const bool server = options.networkRole == SceneNetworkRole::Server;
    try {
        if (!prepare(executable, scene, error) || !impl.group.start(error)) { stop(); return false; }
        const auto child = impl.directory / (server ? "server" : "client");
        if (!std::filesystem::create_directory(child)) {
            error = "Could not create a multiplayer process directory.";
            stop();
            return false;
        }
        if (options.networkSteam && !prepareSteamAppId(impl.binary, child, error)) { stop(); return false; }
        auto details = descriptor(options);
        details["liveScene"] = "";
        const auto reactPackage = impl.binary.parent_path() / "assets/scripts/react-luau";
        if (std::filesystem::is_regular_file(reactPackage / "manifest.tsv")) details["reactPackage"] = utf8(reactPackage);
        details["onlineNetwork"] = {{"role", server ? "server" : "client"}, {"address", options.networkEndpoint.address},
            {"port", options.networkEndpoint.port}, {"host", options.networkHost}, {"steam", options.networkSteam},
            {"steamId", options.networkEndpoint.steamId}, {"capacity", options.networkMaxPeers},
            {"compatibility", options.networkCompatibility}};
        if (!server && !options.networkSteam && !options.networkHostControlDirectory.empty()) {
            details["onlineNetwork"]["localHostControl"] = utf8(options.networkHostControlDirectory);
        }
        const auto manifest = child / "play.json";
        if (!writeFileAtomically(manifest, details.dump(2), error) ||
            !impl.children.front().start(impl.binary, manifest, &impl.group, error)) { stop(); return false; }
        impl.online.role = options.networkRole;
        impl.online.endpoint = options.networkEndpoint;
        impl.online.compatibility = options.networkCompatibility;
        impl.onlineSteam = options.networkSteam;
        impl.nextPoll = Clock::now();
        impl.deadline = impl.nextPoll + 30s;
        return true;
    } catch (const std::exception& failure) { error = failure.what(); stop(); return false; }
}

void PlaySession::pollOnline() {
    auto& impl = *m_impl;
    auto& status = impl.online;
    const auto now = Clock::now();
    if (status.role == SceneNetworkRole::Offline || now < impl.nextPoll) return;
    impl.nextPoll = now + 50ms;
    const bool server = status.role == SceneNetworkRole::Server;
    try {
        if (!impl.children.front().running()) {
            const auto exitCode = impl.children.front().exitCode();
            auto error = exitCode != 0 || !status.ready ?
                childFailure(impl.directory / (server ? "server" : "client"), exitCode) : std::string{};
            stop();
            status.error = std::move(error);
            return;
        }
        if (!status.ready) {
            const auto port = readPort(impl.directory / "ready");
            const auto steamId = server && impl.onlineSteam ? readUnsignedLine(impl.directory / SteamIdMarker).value_or(0)
                                                            : status.endpoint.steamId;
            if (port && (!(server && impl.onlineSteam) || steamId)) {
                status.endpoint.port = *port;
                status.endpoint.steamId = steamId;
                status.ready = true;
            } else if (now >= impl.deadline) {
                stop();
                status.error = "The multiplayer process did not become ready within 30 seconds.";
                return;
            }
        }
        if (status.invitePending) {
            const auto result = readInviteResult(impl.directory / SteamInviteResult, impl.inviteSequence);
            if (result || now >= impl.inviteDeadline) {
                status.invitePending = false;
                status.inviteStatus = !result ? "Steam invitation status timed out." :
                    result->empty() ? "Steam invitation sent." : *result;
            }
        }
        if (server && impl.onlineSteam && status.ready && now >= impl.nextSteamFriendsPoll) {
            impl.nextSteamFriendsPoll = now + 500ms;
            const auto path = impl.directory / net::SteamFriendListFile;
            std::error_code error;
            const auto writeTime = std::filesystem::last_write_time(path, error);
            if (!error && writeTime != impl.steamFriendsWriteTime && readSteamFriends(path, status.friends)) {
                impl.steamFriendsWriteTime = writeTime;
            }
        }
        if (server && impl.onlineSteam && status.ready && now >= impl.nextSteamAvatarPoll) {
            impl.nextSteamAvatarPoll = now + 200ms;
            auto& requested = impl.steamAvatarRequests;
            std::ranges::sort(requested);
            if (requested != impl.publishedAvatarRequests) {
                std::string error;
                if (writeFileAtomically(impl.directory / net::SteamAvatarRequestFile, Json(requested).dump(), error)) {
                    impl.publishedAvatarRequests.swap(requested);
                }
            }
            requested.clear();
        }
    } catch (const std::exception& failure) {
        stop();
        status.error = failure.what();
    }
}

bool PlaySession::inviteOnlineFriend(std::uint64_t friendId, std::string& error) {
    error.clear();
    auto& impl = *m_impl;
    auto& status = impl.online;
    if (status.role != SceneNetworkRole::Server || !status.ready || !status.endpoint.steamId ||
        !impl.children.front().running()) {
        error = "Start a Steam server before inviting friends.";
        return false;
    }
    if (!status.friends || std::ranges::find(*status.friends, friendId, &net::SteamFriend::id) == status.friends->end()) {
        error = "Choose a friend from the current Steam friends list.";
        return false;
    }
    if (status.invitePending || impl.inviteSequence == std::numeric_limits<std::uint64_t>::max()) {
        error = "Another Steam invitation is still pending.";
        return false;
    }
    const auto sequence = impl.inviteSequence + 1;
    if (!writeFileAtomically(impl.directory / SteamInviteRequest,
            std::to_string(sequence) + " " + std::to_string(friendId) + "\n", error)) return false;
    impl.inviteSequence = sequence;
    impl.inviteDeadline = Clock::now() + 10s;
    status.inviteStatus.clear();
    status.invitePending = true;
    return true;
}

std::filesystem::path PlaySession::steamFriendAvatar(std::uint64_t friendId) {
    auto& impl = *m_impl;
    const auto& status = impl.online;
    if (status.role != SceneNetworkRole::Server || !status.ready || !status.endpoint.steamId || !status.friends ||
        std::ranges::find(*status.friends, friendId, &net::SteamFriend::id) == status.friends->end()) return {};

    auto& requested = impl.steamAvatarRequests;
    if (requested.size() < net::MaxSteamAvatarRequests && std::ranges::find(requested, friendId) == requested.end()) {
        requested.push_back(friendId);
    }
    auto& files = impl.steamAvatarFiles;
    if (files.size() >= net::MaxSteamFriends && !files.contains(friendId)) files.clear();
    auto& file = files[friendId];
    const auto now = Clock::now();
    if (now >= file.nextCheck) {
        file.nextCheck = now + 250ms;
        const auto path = impl.directory / net::steamAvatarFileName(friendId);
        std::error_code error;
        if (std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) && !error &&
            std::filesystem::file_size(path, error) == net::SteamAvatarFileBytes && !error) {
            file.path = path;
        } else {
            file.path.clear();
        }
    }
    return file.path;
}

void PlaySession::pollNetwork() {
    auto& impl = *m_impl;
    if (!impl.networkActive) return;
    const auto now = Clock::now();
    if (now < impl.nextPoll) return;
    impl.nextPoll = now + 50ms;
    try {
        if (!impl.children.front().running()) {
            impl.networkError = childFailure(impl.directory / "server", impl.children.front().exitCode());
        } else if (!impl.networkPort) {
            impl.networkPort = readPort(impl.directory / "ready").value_or(0);
            if (!impl.networkPort && now >= impl.deadline) {
                impl.networkError = "Network Play server did not become ready within 30 seconds.";
            }
        }
        if (impl.networkError.empty() && impl.networkPort) {
            const auto controlDirectory = impl.directory / NetworkControlFolder;
            const auto applied = readNetworkTestAppliedProfiles(controlDirectory,
                static_cast<unsigned>(impl.networkProfiles.size()));
            const bool pollTelemetry = now >= impl.nextTelemetryPoll;
            const auto elapsedMs = impl.elapsedMs(now);
            std::optional<std::vector<std::optional<net::NetworkTestTelemetrySample>>> serverTelemetry;
            if (pollTelemetry) {
                impl.nextTelemetryPoll = now + 250ms;
                serverTelemetry = readNetworkTestServerTelemetry(controlDirectory,
                    static_cast<unsigned>(impl.networkProfiles.size()));
                for (auto& control : impl.clientControls) control.history.prune(elapsedMs);
            }
            bool anyClient = false;
            for (std::size_t index = 1; index < impl.nextClient; ++index) {
                const auto& child = impl.children[index];
                auto& control = impl.clientControls[index - 1];
                if (child.running()) {
                    anyClient = true;
                    const auto status = readNetworkTestClientStatus(controlDirectory, static_cast<unsigned>(index));
                    if (status) {
                        const auto previousState = control.state;
                        const auto previousNonce = control.telemetry.nonce;
                        control.state = status->state;
                        const auto revision = impl.networkProfiles[index - 1].revision;
                        control.telemetry.setContext(*status, revision);
                        if (control.state != previousState || control.telemetry.nonce != previousNonce) {
                            control.historyBreak = true;
                        }
                        if (status->nonce && status->revision == revision && applied &&
                            (*applied)[index - 1] == net::NetworkTestAppliedProfile{status->nonce, revision}) {
                            control.confirmedRevision = revision;
                        }
                    }
                    if (pollTelemetry) {
                        const auto clientTelemetry = readNetworkTestClientTelemetry(controlDirectory,
                            static_cast<unsigned>(index));
                        auto measured = control.telemetry.sample(clientTelemetry,
                            serverTelemetry ? (*serverTelemetry)[index - 1] : std::nullopt, control.state, now);
                        control.history.append({elapsedMs, std::move(measured), std::exchange(control.historyBreak, false),
                            impl.networkProfiles[index - 1]});
                    }
                } else {
                    if (control.state != net::NetworkTestLinkState::Closed) {
                        control.state = net::NetworkTestLinkState::Closed;
                        control.telemetry = {};
                    }
                    if (const auto code = child.exitCode(); code != 0) {
                        impl.networkError = childFailure(impl.directory / ("client-" + std::to_string(index)), code);
                        break;
                    }
                }
            }
            const auto clientCount = static_cast<std::size_t>(impl.networkOptions.clientCount);
            if (impl.networkError.empty() && impl.nextClient > clientCount && !anyClient) { stop(); return; }
            constexpr unsigned ClientsPerPoll = 2;
            for (unsigned i = 0; i < ClientsPerPoll && impl.networkError.empty() && impl.nextClient <= clientCount; ++i) {
                if (!impl.startChild(impl.nextClient, impl.networkPort, impl.networkError)) break;
                ++impl.nextClient;
            }
        }
    } catch (const std::exception& failure) { impl.networkError = failure.what(); }
    if (!impl.networkError.empty()) {
        auto failure = std::move(impl.networkError);
        stop();
        impl.networkError = std::move(failure);
    }
}

std::optional<PlayLaunch> PlaySession::load(const std::filesystem::path& manifest, std::string& error) {
    error.clear();
    try {
        std::ifstream input(manifest);
        const auto value = nlohmann::json::parse(input);
        if (value.at("version") != 1) { error = "Unsupported Play session version."; return {}; }
        PlayLaunch launch;
        launch.project.name = value.at("name").get<std::string>();
        launch.project.root = fromUtf8(value.at("root").get<std::string>());
        launch.project.assetRoot = fromUtf8(value.at("assets").get<std::string>());
        const auto limit = readBoundedInteger(value, "networkPlayerLimit", 0, static_cast<int>(net::MaxPeers), 0);
        if (!limit) { error = "Invalid project network player limit."; return {}; }
        launch.project.networkPlayerLimit = static_cast<unsigned>(*limit);
        const auto directory = std::filesystem::absolute(manifest).parent_path();
        launch.project.scenePath = directory / "scene.json";
        launch.project.liveScenePath = fromUtf8(value.value("liveScene", std::string{}));
        launch.project.layoutPath = directory / "editor_layout.json";
        launch.width = value.at("width").get<int>();
        launch.height = value.at("height").get<int>();
        launch.vsync = value.at("vsync").get<bool>();
        launch.device = value.at("device").get<std::string>();
        if (const auto online = value.find("onlineNetwork"); online != value.end()) {
            const auto role = online->at("role").get<std::string>();
            const auto address = readUnsignedField(*online, "address");
            const auto port = readBoundedInteger(*online, "port", 0, 65535);
            const auto steamId = readUnsignedField(*online, "steamId");
            const auto capacity = readBoundedInteger(*online, "capacity", 1, static_cast<int>(net::MaxPeers));
            const auto compatibility = readUnsignedField(*online, "compatibility");
            if (value.contains("network") || (role != "server" && role != "client") ||
                !address || *address > std::numeric_limits<std::uint32_t>::max() || !port ||
                !steamId || !capacity || !compatibility) {
                error = "Invalid online game manifest.";
                return {};
            }
            launch.networkRole = role == "server" ? SceneNetworkRole::Server : SceneNetworkRole::Client;
            launch.networkEndpoint = {static_cast<std::uint32_t>(*address), static_cast<std::uint16_t>(*port), *steamId};
            launch.networkHost = online->at("host").get<std::string>();
            launch.networkSteam = online->at("steam").get<bool>();
            launch.networkMaxPeers = static_cast<unsigned>(*capacity);
            launch.networkCompatibility = *compatibility;
            if (!validOnlineLaunch(launch) || (launch.networkRole == SceneNetworkRole::Server && *limit && *capacity > *limit)) {
                error = "Invalid multiplayer role, endpoint or scene identity.";
                return {};
            }
            const auto session = directory.parent_path();
            launch.project.scenePath = session / "scene.json";
            launch.project.liveScenePath.clear();
            launch.reactPackage = fromUtf8(value.value("reactPackage", std::string{}));
            launch.networkReadyPath = session / "ready";
            if (launch.networkRole == SceneNetworkRole::Server) launch.networkStopPath = session / "stop";
            if (!launch.networkSteam) {
                if (launch.networkRole == SceneNetworkRole::Server) launch.networkHostControlDirectory = session;
                else {
                    auto control = fromUtf8(online->value("localHostControl", std::string{}));
                    if (control.is_absolute()) launch.networkHostControlDirectory = std::move(control);
                }
            }
        } else if (const auto network = value.find("network"); network != value.end()) {
            const auto role = network->at("role").get<std::string>();
            auto& options = launch.networkOptions;
            const auto count = readBoundedInteger(*network, "clientCount", 1, net::MaxNetworkTestClients, options.clientCount);
            const auto latency = readBoundedInteger(*network, "latencyMs", 0, net::MaxNetworkTestLatencyMs, options.latencyMs);
            const auto loss = readBoundedInteger(*network, "lossPercent", 0, net::MaxNetworkTestLossPercent, options.lossPercent);
            if (!count || !latency || !loss || (*limit && *count > *limit)) {
                error = invalidNetworkOptions();
                return {};
            }
            options = {*count, *latency, *loss};
            const auto port = readBoundedInteger(*network, "port", 0, 65535);
            const auto client = readBoundedInteger(*network, "client", 0, *count);
            if (!port || !client || (role == "server" && *client != 0) ||
                (role == "client" && (*port == 0 || *client == 0)) ||
                (role != "server" && role != "client")) {
                error = "Invalid Network Play role, port or client index.";
                return {};
            }
            const auto session = directory.parent_path();
            launch.networkRole = role == "server" ? SceneNetworkRole::Server : SceneNetworkRole::Client;
            launch.networkEndpoint.port = static_cast<std::uint16_t>(*port);
            launch.networkClientIndex = static_cast<unsigned>(*client);
            launch.project.scenePath = session / "scene.json";
            launch.project.liveScenePath.clear();
            if (const auto live = network->find("liveControl"); live != network->end()) {
                if (!live->is_boolean()) {
                    error = "Network Play liveControl must be a boolean.";
                    return {};
                }
                if (live->get<bool>()) launch.networkControlDirectory = session / NetworkControlFolder;
            }
            if (launch.networkRole == SceneNetworkRole::Server) {
                launch.networkReadyPath = session / "ready";
                launch.networkStopPath = session / "stop";
            }
        }
        if (launch.width < 320 || launch.height < 200 || launch.width > 16384 || launch.height > 16384 ||
            !std::filesystem::is_directory(launch.project.assetRoot) || !std::filesystem::is_regular_file(launch.project.scenePath)) {
            error = "Play session has invalid dimensions or missing scene/assets."; return {};
        }
        return launch;
    } catch (const std::exception& failure) { error = failure.what(); return {}; }
}
}
