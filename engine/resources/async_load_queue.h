#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine/core/JobSystem.h"

namespace engine::resources {

struct AssetLoadingStats final {
    std::size_t submitted{0};
    std::size_t pendingCpu{0};
    std::size_t pendingUploads{0};
    std::size_t loaded{0};
    std::size_t failed{0};
    std::size_t cancelled{0};
    std::size_t uploadedLastFrame{0};
    double lastPumpMilliseconds{0.0};
    bool asyncEnabled{true};
};

// Called from the renderer/main thread only. Work may read/decode CPU data;
// finalize and onError always run on the owning thread. The shared JobSystem
// must outlive this queue. stop() joins CPU work and cancels pending uploads.
class AsyncLoadQueue final {
public:
    using Finalize = std::function<bool()>;
    using Work = std::function<Finalize()>;
    using OnError = std::function<void(const std::string&)>;

    AsyncLoadQueue();
    ~AsyncLoadQueue();
    AsyncLoadQueue(const AsyncLoadQueue&) = delete;
    AsyncLoadQueue& operator=(const AsyncLoadQueue&) = delete;

    void start(core::JobSystem* jobs);
    void setAsyncEnabled(bool enabled);
    void setUploadBudget(std::size_t maximumUploads, double milliseconds);
    void submit(Work work, OnError onError);
    void pump();
    void stop();
    [[nodiscard]] AssetLoadingStats stats() const;

private:
    struct State;
    struct Completion;
    struct Pending final {
        Work work;
        std::shared_ptr<Completion> completion;
    };
    std::shared_ptr<State> m_state;
    core::JobSystem* m_jobs{nullptr};
    std::vector<core::JobSystem::Handle> m_handles;
    std::deque<Pending> m_waiting;
    std::deque<std::shared_ptr<Completion>> m_completions;
    std::thread::id m_owner;
    std::size_t m_maximumUploads{1};
    double m_uploadBudgetMilliseconds{2.0};
    bool m_asyncEnabled{true};
    bool m_stopped{false};

    void requireOwner() const;
    void dispatchPending();
    void finalize(const std::shared_ptr<Completion>& completion);
};

} // namespace engine::resources
