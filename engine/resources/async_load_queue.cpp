#include "engine/resources/async_load_queue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <exception>
#include <stdexcept>
#include <utility>

#include "engine/core/Profiling.h"

namespace engine::resources {
namespace {

std::string currentError() {
    try {
        throw;
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "Unknown asset loading exception";
    }
}

void reportError(const AsyncLoadQueue::OnError& callback, const std::string& error) noexcept {
    try {
        if (callback) {
            callback(error);
        }
    } catch (...) {
        // Shutdown/error cleanup cannot escape through a renderer destructor.
    }
}

} // namespace

struct AsyncLoadQueue::Completion final {
    Finalize finalize;
    OnError onError;
    std::exception_ptr failure;
    std::atomic_bool ready{false};
};

struct AsyncLoadQueue::State final {
    std::atomic_size_t pendingCpu{0};
    AssetLoadingStats counters;
};

AsyncLoadQueue::AsyncLoadQueue()
    : m_state(std::make_shared<State>()), m_owner(std::this_thread::get_id()) {}

AsyncLoadQueue::~AsyncLoadQueue() {
    stop();
}

void AsyncLoadQueue::requireOwner() const {
    if (std::this_thread::get_id() != m_owner) {
        throw std::logic_error("Asset requests, pump and shutdown belong to the renderer thread");
    }
}

void AsyncLoadQueue::start(core::JobSystem* jobs) {
    requireOwner();
    stop();
    m_jobs = jobs;
    m_state = std::make_shared<State>();
    m_stopped = false;
}

void AsyncLoadQueue::setAsyncEnabled(const bool enabled) {
    requireOwner();
    m_asyncEnabled = enabled;
}

void AsyncLoadQueue::setUploadBudget(const std::size_t maximumUploads, const double milliseconds) {
    requireOwner();
    m_maximumUploads = std::max<std::size_t>(1U, maximumUploads);
    m_uploadBudgetMilliseconds = std::isfinite(milliseconds) && milliseconds > 0.0 ? milliseconds : 2.0;
}

void AsyncLoadQueue::submit(Work work, OnError onError) {
    ENGINE_PROFILE_ZONE("Asset Dispatch");
    requireOwner();
    ++m_state->counters.submitted;
    if (m_stopped || !work) {
        ++m_state->counters.failed;
        reportError(onError, m_stopped ? "Asset loading stopped" : "Empty asset loading task");
        return;
    }

    if (!m_asyncEnabled || m_jobs == nullptr || !m_jobs->enabled()) {
        ENGINE_PROFILE_ZONE("Asset Synchronous Load");
        auto completion = std::make_shared<Completion>();
        completion->onError = std::move(onError);
        try {
            completion->finalize = work();
        } catch (...) {
            completion->failure = std::current_exception();
        }
        finalize(completion);
        return;
    }

    auto completion = std::make_shared<Completion>();
    completion->onError = std::move(onError);
    m_completions.push_back(completion);
    try {
        m_waiting.push_back(Pending{std::move(work), completion});
    } catch (...) {
        completion->failure = std::current_exception();
        completion->ready.store(true, std::memory_order_release);
    }
    dispatchPending();
}

void AsyncLoadQueue::dispatchPending() {
    if (m_stopped || m_jobs == nullptr) {
        return;
    }
    std::erase_if(m_handles, [this](const auto& handle) { return m_jobs->isComplete(handle); });
    // Keep far below enkiTS's task-pipe capacity: a saturated pipe may execute
    // work inline in dispatch(), which would run asset IO on the render thread.
    const auto limit = std::clamp<std::size_t>(m_jobs->workerCount() * 2U, 1U, 16U);
    // Include decoded-but-not-uploaded records in the limit. This also bounds
    // decoded staging memory while a slow GPU pump catches up.
    while (!m_waiting.empty() && m_completions.size() - m_waiting.size() < limit) {
        auto request = std::move(m_waiting.front());
        m_waiting.pop_front();
        const auto completion = std::move(request.completion);
        const auto state = m_state;
        state->pendingCpu.fetch_add(1U, std::memory_order_relaxed);
        try {
            if (m_handles.size() == m_handles.capacity()) {
                m_handles.reserve(std::max<std::size_t>(16U, m_handles.size() * 2U));
            }
            m_handles.push_back(m_jobs->dispatch(
                [state, work = std::move(request.work), completion]() mutable {
                    ENGINE_PROFILE_ZONE("Asset Decode Job");
                    try {
                        completion->finalize = work();
                    } catch (...) {
                        completion->failure = std::current_exception();
                    }
                    // Preallocated on the main thread: no queue allocation can
                    // throw here or lose a completion/pendingCpu decrement.
                    completion->ready.store(true, std::memory_order_release);
                    state->pendingCpu.fetch_sub(1U, std::memory_order_release);
                }, core::JobSystem::Priority::Low));
        } catch (...) {
            state->pendingCpu.fetch_sub(1U, std::memory_order_release);
            completion->failure = std::current_exception();
            completion->ready.store(true, std::memory_order_release);
        }
    }
}

void AsyncLoadQueue::finalize(const std::shared_ptr<Completion>& completion) {
    ENGINE_PROFILE_ZONE("Asset GPU Finalize");
    bool loaded = false;
    std::string error;
    try {
        if (completion->failure) {
            std::rethrow_exception(completion->failure);
        }
        if (!completion->finalize) {
            throw std::runtime_error("Asset job returned no finalization callback");
        }
        loaded = completion->finalize();
    } catch (...) {
        error = currentError();
    }
    if (loaded) {
        ++m_state->counters.loaded;
    } else {
        ++m_state->counters.failed;
        if (!error.empty()) {
            reportError(completion->onError, error);
        }
    }
}

void AsyncLoadQueue::pump() {
    ENGINE_PROFILE_ZONE("Asset Main Thread Tasks");
    requireOwner();
    const auto started = std::chrono::steady_clock::now();
    m_state->counters.uploadedLastFrame = 0;
    dispatchPending();
    while (!m_stopped && m_state->counters.uploadedLastFrame < m_maximumUploads) {
        const auto ready = std::find_if(m_completions.begin(), m_completions.end(), [](const auto& completion) {
            return completion->ready.load(std::memory_order_acquire);
        });
        if (ready == m_completions.end()) {
            break;
        }
        auto completion = std::move(*ready);
        m_completions.erase(ready);
        finalize(completion);
        ++m_state->counters.uploadedLastFrame;
        // An individual driver upload is non-preemptible. The budget prevents
        // starting another upload, but cannot interrupt a slow GPU API call.
        const double elapsed = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - started).count();
        if (elapsed >= m_uploadBudgetMilliseconds) {
            break;
        }
    }
    m_state->counters.lastPumpMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    const auto current = stats();
    ENGINE_PROFILE_PLOT("Assets Pending CPU", static_cast<std::int64_t>(current.pendingCpu));
    ENGINE_PROFILE_PLOT("Assets Pending Uploads", static_cast<std::int64_t>(current.pendingUploads));
    ENGINE_PROFILE_PLOT("Assets Loaded", static_cast<std::int64_t>(current.loaded));
    ENGINE_PROFILE_PLOT("Assets Failed", static_cast<std::int64_t>(current.failed));
}

void AsyncLoadQueue::stop() {
    ENGINE_PROFILE_ZONE("Asset Shutdown Join");
    requireOwner();
    if (m_stopped) {
        return;
    }
    m_stopped = true;
    if (m_jobs != nullptr) {
        for (const auto& handle : m_handles) {
            try {
                m_jobs->wait(handle);
            } catch (...) {
                // Decoder exceptions already travel through Completion.failure.
                // Keep joining all other jobs before renderer data is destroyed.
            }
        }
    }
    m_handles.clear();
    m_waiting.clear();
    for (const auto& completion : m_completions) {
        reportError(completion->onError, "Asset loading cancelled during shutdown");
        ++m_state->counters.cancelled;
    }
    m_completions.clear();
}

AssetLoadingStats AsyncLoadQueue::stats() const {
    requireOwner();
    auto result = m_state->counters;
    result.pendingCpu = m_state->pendingCpu.load(std::memory_order_acquire) + m_waiting.size();
    result.pendingUploads = static_cast<std::size_t>(std::count_if(
        m_completions.begin(), m_completions.end(), [](const auto& completion) {
            return completion->ready.load(std::memory_order_acquire);
        }));
    result.asyncEnabled = m_asyncEnabled && m_jobs != nullptr && m_jobs->enabled();
    return result;
}

} // namespace engine::resources
