#include "engine/core/Time.h"

#include <algorithm>

namespace engine::core {

FrameTimer::FrameTimer() : m_lastTick(Clock::now()) {}

void FrameTimer::reset() {
    m_lastTick = Clock::now();
}

double FrameTimer::tickSeconds() {
    const Clock::time_point now = Clock::now();
    const std::chrono::duration<double> elapsed = now - m_lastTick;
    m_lastTick = now;
    return elapsed.count();
}

TimeStatsAggregator::TimeStatsAggregator(double emitIntervalSeconds)
    : m_emitIntervalSeconds(emitIntervalSeconds) {}

void TimeStatsAggregator::sample(double deltaSeconds) {
    m_accumulatedSeconds += deltaSeconds;
    m_sumDelta += deltaSeconds;
    m_samples += 1;

    if (m_samples == 1) {
        m_minDelta = deltaSeconds;
        m_maxDelta = deltaSeconds;
    } else {
        m_minDelta = std::min(m_minDelta, deltaSeconds);
        m_maxDelta = std::max(m_maxDelta, deltaSeconds);
    }

    if (m_accumulatedSeconds >= m_emitIntervalSeconds) {
        m_cached.sampleCount = m_samples;
        m_cached.intervalSeconds = m_accumulatedSeconds;
        m_cached.avgMs = (m_sumDelta / static_cast<double>(m_samples)) * 1000.0;
        m_cached.minMs = m_minDelta * 1000.0;
        m_cached.maxMs = m_maxDelta * 1000.0;
        m_ready = true;

        m_accumulatedSeconds = 0.0;
        m_sumDelta = 0.0;
        m_minDelta = 0.0;
        m_maxDelta = 0.0;
        m_samples = 0;
    }
}

bool TimeStatsAggregator::consume(TimeStats& outStats) {
    if (!m_ready) {
        return false;
    }
    outStats = m_cached;
    m_ready = false;
    return true;
}

} // namespace engine::core
