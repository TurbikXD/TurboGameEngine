#include "app/Lab5Session.h"

#include <array>
#include <cstdlib>
#include <utility>

#include "engine/core/CrashReport.h"
#include "engine/core/Log.h"
#include "engine/game/GameplayState.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace engine::core {
namespace {

struct Probe {
    std::string_view name;
    int minimumRevision;
    void (*trigger)();
};

#if defined(_WIN32)
[[noreturn]] __declspec(noinline) void lab5IntegerDivideByZero() {
    RaiseException(EXCEPTION_INT_DIVIDE_BY_ZERO, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();
}

[[noreturn]] __declspec(noinline) void lab5AccessViolation() {
    const ULONG_PTR parameters[]{1, 0};
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, parameters);
    std::abort();
}

[[noreturn]] __declspec(noinline) void lab5AssertionFailure() {
    Log::engineLogger()->critical("Assertion failed: Lab 5 diagnostic assertion");
    RaiseException(0xC0000420UL, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();
}

constexpr std::array probes{
    Probe{"divide-by-zero", 1, lab5IntegerDivideByZero},
    Probe{"access-violation", 1, lab5AccessViolation},
    Probe{"assertion-failure", 2, lab5AssertionFailure},
};
#else
constexpr std::array<Probe, 0> probes{};
#endif

const Probe* findProbe(std::string_view name) {
    for (const auto& probe : probes) {
        if (probe.name == name && probe.minimumRevision <= Lab5Session::revision) return &probe;
    }
    return nullptr;
}

}

bool Lab5Session::acceptsVerification(std::string_view name) {
    return name == "smoke" || findProbe(name) != nullptr;
}

Lab5Session::Lab5Session(std::string verification) : m_verification(std::move(verification)) {}

void Lab5Session::attach(game::GameplayState& game) {
    m_game = &game;
    m_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
}

std::optional<int> Lab5Session::afterFrame() {
    const auto& statuses = m_game->scriptStatus();
    const auto status = statuses.find("lab5");
    if ((!m_configured || verifying()) && std::chrono::steady_clock::now() >= m_deadline) {
        ENGINE_LOG_ERROR("Lab 5 timed out waiting for scripts/Beacon.luau ({}/3 channels requested)", m_requestedChannel);
        return EXIT_FAILURE;
    }
    if (status == statuses.end() || !status->is_object() || status->find("ready") == status->end() ||
        (*status)["ready"] != true) return {};

    if (const auto lastAction = status->find("lastAction"); lastAction != status->end() && lastAction->is_string()) {
        const auto& action = lastAction->get_ref<const std::string&>();
        if (action != m_lastAction && action.size() <= 80) {
            m_lastAction = action;
            CrashReport::setAction(action);
            ENGINE_LOG_INFO("LAB5_ACTION {}", action);
        }
    }
    if (!m_configured) {
        m_game->queueScriptEvent("Lab5Control", {{"command", "configure"}, {"buildId", ENGINE_BUILD_ID},
                                                {"demoRevision", revision}});
        m_configured = true;
        ENGINE_LOG_INFO("LAB5_READY scene=beacon.scene.json buildId={} revision={}", ENGINE_BUILD_ID, revision);
        return {};
    }

    const auto channels = status->find("channels");
    if (channels == status->end() || !channels->is_number()) return {};
    if (*channels == 3) {
        if (!m_calibrated) {
            m_calibrated = true;
            ENGINE_LOG_INFO("LAB5_CALIBRATED channels=3");
        }
        if (const auto request = status->find("crashRequest"); request != status->end() && request->is_string()) {
            const auto& scenario = request->get_ref<const std::string&>();
            if (const auto* probe = findProbe(scenario); probe && (!verifying() || m_verification == scenario)) {
                CrashReport::setAction("probe-" + scenario);
                ENGINE_LOG_INFO("LAB5_PROBE scenario={}", scenario);
                probe->trigger();
                return EXIT_FAILURE;
            }
        }
    } else {
        m_calibrated = false;
    }

    if (!verifying() || *channels != m_requestedChannel) return {};
    if (m_requestedChannel < 3) {
        m_game->queueScriptEvent("Lab5Control", {{"command", "calibrate"}, {"channel", ++m_requestedChannel}});
    } else if (m_verification == "smoke") {
        ENGINE_LOG_INFO("LAB5_SMOKE_OK");
        return EXIT_SUCCESS;
    } else if (!m_requestedCrash) {
        m_game->queueScriptEvent("Lab5Control", {{"command", "crash"}, {"scenario", m_verification}});
        m_requestedCrash = true;
    }
    return {};
}

}
