#pragma once

#include <chrono>

namespace engine::core {

class FrameTimer final {
public:
    using Clock = std::chrono::steady_clock;

    FrameTimer();
    void reset();
    double tickSeconds();

private:
    Clock::time_point m_lastTick;
};

struct TimeStats final {
    double avgMs{0.0};
    double minMs{0.0};
    double maxMs{0.0};
    double intervalSeconds{0.0};
    int sampleCount{0};
};

class TimeStatsAggregator final {
public:
    explicit TimeStatsAggregator(double emitIntervalSeconds = 1.0);
    void sample(double deltaSeconds);
    bool consume(TimeStats& outStats);

private:
    double m_emitIntervalSeconds;
    double m_accumulatedSeconds{0.0};
    double m_sumDelta{0.0};
    double m_minDelta{0.0};
    double m_maxDelta{0.0};
    int m_samples{0};
    bool m_ready{false};
    TimeStats m_cached{};
};

} // namespace engine::core
