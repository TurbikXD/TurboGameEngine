#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace engine::game { class GameplayState; }

namespace engine::core {

class Lab5Session final {
public:
    static constexpr int revision = 2;
    static bool acceptsVerification(std::string_view name);

    explicit Lab5Session(std::string verification);
    void attach(game::GameplayState& game);
    [[nodiscard]] bool verifying() const { return !m_verification.empty(); }
    std::optional<int> afterFrame();

private:
    std::string m_verification;
    game::GameplayState* m_game{};
    std::chrono::steady_clock::time_point m_deadline;
    std::string m_lastAction;
    int m_requestedChannel{};
    bool m_configured{};
    bool m_calibrated{};
    bool m_requestedCrash{};
};

}
