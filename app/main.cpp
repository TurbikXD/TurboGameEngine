#include "app/Lab5Session.h"
#if defined(_WIN32)
#include "app/CrashReportClient.h"
#endif
#include "engine/core/Application.h"
#include "engine/core/AtomicFile.h"
#include "engine/core/CrashReport.h"
#include "engine/core/Log.h"
#include "engine/rhi/RHI.h"
#include "engine/core/PlaySession.h"
#include "engine/game/NetworkArena.h"
#include "engine/game/SceneNetworkServer.h"
#include "engine/network/SteamTransport.h"
#include <algorithm>
#include <charconv>
#include <iostream>
#include <exception>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {
std::string utf8Argument(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}

std::optional<std::uint64_t> parseNumber(std::string_view text) {
    std::uint64_t value{};
    const auto* end = text.data() + text.size();
    const auto [position, error] = std::from_chars(text.data(), end, value);
    if (position != end || error != std::errc{}) return {};
    return value;
}

int usage() {
    std::cerr << "Usage: app [--play-session manifest | --net-server [IPv4:port] | --net-client [IPv4:port]]\n"
                 "       app --build-info\n"
#if defined(_WIN32)
                 "       app [--crash-report | --send-crash-report] <report directory>\n"
#endif
                 "       app --lab5 <Lab5Beacon.tgeproj> [--verify smoke|divide-by-zero|access-violation|assertion-failure]\n"
                 "       app --scene-server <project.tgeproj> [bind IPv4[:port]] [--max-clients N]\n"
                 "       app --scene-client <project.tgeproj> <host[:port]>\n"
                 "       app --steam-server <project.tgeproj> [--max-clients N] [--invite friendSteamID]\n"
                 "       app --steam-client <project.tgeproj> [hostSteamID | +tge_join payload]\n"
                 "       app --steam-game <project.tgeproj>\n";
    return 2;
}

template<class Char>
bool parseSceneServerArguments(int argc, Char** argv, engine::core::PlayLaunch& launch, std::string& error) {
    using namespace engine::net;
    const unsigned playerLimit = launch.project.networkPlayerLimit ? launch.project.networkPlayerLimit : static_cast<unsigned>(MaxPeers);
    launch.networkMaxPeers = std::min(playerLimit, static_cast<unsigned>(ServerConfig{}.maxPeers));
    launch.networkEndpoint = {0x7f000001, DefaultPort};
    int next = 3;
    if (!launch.networkSteam && next < argc) {
        const auto argument = utf8Argument(argv[next]);
        if (!argument.starts_with("--")) {
            const auto endpoint = parseEndpoint(argument, DefaultPort);
            if (!endpoint || endpoint->port == 0 || endpoint->address >= 0xe0000000U) {
                error = "Invalid bind endpoint. Use a local IPv4 address and a UDP port from 1 to 65535.";
                return false;
            }
            launch.networkEndpoint = *endpoint;
            ++next;
        }
    }
    bool maxClientsSet = false;
    for (; next < argc; next += 2) {
        const auto option = utf8Argument(argv[next]);
        const auto value = next + 1 < argc ? parseNumber(utf8Argument(argv[next + 1])) : std::nullopt;
        if (option == "--max-clients" && !maxClientsSet) {
            if (!value || *value == 0 || *value > playerLimit) {
                error = "--max-clients must be between 1 and " + std::to_string(playerLimit) + " for this project.";
                return false;
            }
            launch.networkMaxPeers = static_cast<unsigned>(*value);
            maxClientsSet = true;
        } else if (launch.networkSteam && option == "--invite" && !launch.networkSteamInvite) {
            if (!value || !validSteamUserId(*value)) {
                error = "--invite requires a valid friend's SteamID64.";
                return false;
            }
            launch.networkSteamInvite = *value;
        } else {
            error = "Unknown or repeated server option: " + option;
            return false;
        }
    }
    return true;
}

}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    using namespace engine::core;
#if defined(_WIN32)
    if (argc > 1) {
        const std::wstring_view command{argv[1]};
        if (command == L"--crash-report" || command == L"--send-crash-report") {
            if (argc != 3) return usage();
            return engine::app::runCrashReportClient(argv[2], command == L"--send-crash-report");
        }
    }
#endif
    CrashReport::install(ENGINE_BUILD_ID, ENGINE_BUILD_COMMIT);
    const struct DiagnosticsScope {
        ~DiagnosticsScope() { CrashReport::shutdown(); }
    } diagnostics;
    try {
        std::optional<PlayLaunch> standalone;
        std::optional<engine::net::Endpoint> networkArena;
        std::optional<Lab5Session> lab5;
#if defined(_WIN32)
        bool offerCrashReport = true;
#endif
        if (argc > 1) {
            const std::filesystem::path mode{argv[1]};
            const bool sceneServer = mode == "--scene-server" || mode == "--steam-server";
            const bool steamGame = mode == "--steam-game";
            const bool sceneClient = mode == "--scene-client" || mode == "--steam-client" || steamGame;
            if (argc == 2 && mode == "--build-info") {
                std::cout << nlohmann::json{{"schemaVersion", 1}, {"buildId", ENGINE_BUILD_ID},
                    {"commitSha", ENGINE_BUILD_COMMIT}, {"demoRevision", Lab5Session::revision}}.dump() << '\n';
                return 0;
#if defined(_WIN32)
            } else if (mode == "--lab5") {
                if (argc != 3 && (argc != 5 || utf8Argument(argv[3]) != "--verify")) return usage();
                auto verification = argc == 5 ? utf8Argument(argv[4]) : std::string{};
                if (argc == 5 && !Lab5Session::acceptsVerification(verification)) {
                    std::cerr << "Unsupported Lab 5 verification for this build: " << verification << '\n';
                    return 2;
                }
                std::string error;
                auto project = ProjectManager::load(std::filesystem::path(argv[2]), error);
                if (!project) { std::cerr << "Invalid Lab 5 project: " << error << '\n'; return 2; }
                for (const auto* resource : {"scripts/Beacon.luau", "textures/beacon.ppm"}) {
                    std::error_code fileError;
                    if (!std::filesystem::is_regular_file(project->assetRoot / resource, fileError)) {
                        std::cerr << "Missing Lab 5 resource: " << resource << '\n';
                        return 2;
                    }
                }
                standalone.emplace();
                standalone->project = std::move(*project);
                standalone->device = "d3d11";
                standalone->vsync = verification.empty();
                lab5.emplace(std::move(verification));
#endif
            } else if (argc == 3 && mode == "--play-session") {
                std::string error;
                standalone = PlaySession::load(std::filesystem::path(argv[2]), error);
                if (!standalone) { std::cerr << "Invalid Play session: " << error << '\n'; return 2; }
#if defined(_WIN32)
                offerCrashReport = standalone->networkRole == SceneNetworkRole::Offline;
#endif
                if (standalone->networkRole == SceneNetworkRole::Server) {
                    CrashReport::setScene(utf8Argument(standalone->project.scenePath.filename()));
                    return engine::game::runNetworkSceneServer(*standalone);
                }
                if (!standalone->networkHost.empty()) {
                    const auto endpoint = engine::net::resolveConnectEndpoint(standalone->networkHost,
                        engine::net::DefaultPort, error);
                    if (!endpoint) {
                        const auto message = "Could not resolve server: " + error;
                        if (!standalone->networkReadyPath.empty()) {
                            std::string ignored;
                            engine::core::writeFileAtomically(standalone->networkReadyPath.parent_path() /
                                "error.txt", message + "\n", ignored);
                        }
                        std::cerr << message << '\n';
                        return 2;
                    }
                    standalone->networkEndpoint = *endpoint;
                }
            } else if (sceneServer || sceneClient) {
                using namespace engine;
                if (argc < 3 || (steamGame && argc != 3) || (mode == "--scene-client" && argc != 4) ||
                    (mode == "--steam-client" && (argc > 5 || (argc == 5 && utf8Argument(argv[3]) != net::SteamJoinCommand)))) {
                    return usage();
                }
                core::PlayLaunch launch;
                launch.networkSteam = mode == "--steam-server" || mode == "--steam-client" || steamGame;
                launch.networkRole = sceneServer ? core::SceneNetworkRole::Server : core::SceneNetworkRole::Client;
                std::string error;
                auto project = core::ProjectManager::load(std::filesystem::path(argv[2]), error, &launch.networkCompatibility);
                if (!project) { std::cerr << "Invalid network project: " << error << '\n'; return 2; }
                launch.project = std::move(*project);
                if (sceneServer) {
                    CrashReport::setScene(utf8Argument(launch.project.scenePath.filename()));
                    if (!parseSceneServerArguments(argc, argv, launch, error)) {
                        std::cerr << error << '\n';
                        return 2;
                    }
                    if (launch.networkSteam) {
                        net::SteamInvites steam;
                        if (!steam.start(error)) { std::cerr << "Steam initialization failed: " << error << '\n'; return 1; }
                        launch.networkEndpoint = {0, net::SteamVirtualPort, steam.localUserId()};
                        return game::runNetworkSceneServer(launch, &steam);
                    }
                    return game::runNetworkSceneServer(launch);
                }
                launch.networkMaxPeers = static_cast<unsigned>(net::MaxPeers);
                if (launch.networkSteam) {
                    launch.networkEndpoint = {0, net::SteamVirtualPort};
                    if (argc == 4) {
                        const auto hostId = parseNumber(utf8Argument(argv[3]));
                        if (!hostId || !net::validSteamUserId(*hostId)) {
                            std::cerr << "Invalid host SteamID64.\n";
                            return 2;
                        }
                        launch.networkEndpoint.steamId = *hostId;
                    } else if (argc == 5) {
                        const auto join = net::parseSteamJoin(utf8Argument(argv[4]));
                        if (!join || join->compatibilityId != launch.networkCompatibility) {
                            std::cerr << "The Steam invitation is invalid or for a different project scene.\n";
                            return 2;
                        }
                        launch.networkEndpoint.steamId = join->hostId;
                    }
                } else {
                    const auto endpoint = net::resolveConnectEndpoint(utf8Argument(argv[3]), net::DefaultPort, error);
                    if (!endpoint) { std::cerr << "Could not resolve server: " << error << '\n'; return 2; }
                    launch.networkEndpoint = *endpoint;
                }
                standalone = std::move(launch);
            } else if (argc <= 3 && (mode == "--net-server" || mode == "--net-client")) {
                engine::net::Endpoint endpoint{0x7f000001, engine::net::DefaultPort};
                if (argc == 3) {
                    auto address = utf8Argument(argv[2]);
                    if (!address.empty() && address.find_first_not_of("0123456789") == std::string::npos) {
                        address.insert(0, "127.0.0.1:");
                    }
                    const auto parsed = engine::net::parseEndpoint(address, engine::net::DefaultPort);
                    if (!parsed || parsed->port == 0 || (mode == "--net-client" && parsed->address == 0)) {
                        std::cerr << "Invalid network endpoint. Use a numeric IPv4 address and port.\n";
                        return 2;
                    }
                    endpoint = *parsed;
                }
                CrashReport::setScene("Relay Arena");
                if (mode == "--net-server") return engine::game::runNetworkArenaServer(endpoint);
                networkArena = endpoint;
            } else {
                return usage();
            }
        }
        if (standalone) CrashReport::setScene(utf8Argument(standalone->project.scenePath.filename()));
#if defined(_WIN32)
        if (offerCrashReport && (!lab5 || !lab5->verifying())) engine::app::enableCrashReportDialog();
#endif
        constexpr const char* backendName = ENGINE_BACKEND_NAME;
        const engine::rhi::BackendType backend = engine::rhi::backendFromString(backendName);

        Application app(backend, std::move(standalone), networkArena, lab5 ? &*lab5 : nullptr);
        if (!app.init()) {
            ENGINE_LOG_ERROR("App initialization failed");
            return 1;
        }

        return app.run();
    } catch (const std::exception& error) {
        CrashReport::fatal(error.what());
        return 1;
    } catch (...) {
        CrashReport::fatal("Unhandled non-standard C++ exception");
        return 1;
    }
}
