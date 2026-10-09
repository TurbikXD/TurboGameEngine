#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine/core/JobSystem.h"
#include "engine/rhi/UploadQueue.h"

namespace engine::resources {

struct AssetLoadingStats final {
    std::size_t submitted{0};
    std::size_t pendingCpu{0};
    std::size_t pendingUploads{0};
    std::size_t pendingGpu{0}; // subset of pendingUploads waiting for a GPU fence
    std::size_t gpuUploads{0};
    std::uint64_t gpuBytes{0};
    std::uint64_t lastGpuFence{0};
    std::size_t loaded{0};
    std::size_t failed{0};
    std::size_t cancelled{0};
    std::size_t uploadedLastFrame{0};
    double lastPumpMilliseconds{0.0};
    bool asyncEnabled{true};
    bool gpuUploadEnabled{false};
    std::size_t inFlightLimit{0};
};

// Called from the renderer/main thread only. Work may decode and submit to a
// separate GPU transfer context. finalize/onError always run on the owner;
// finalize is gated by an optional real GPU ticket. The shared JobSystem and
// device must outlive this queue. stop() joins CPU and GPU, then cancels backlog.
class AsyncLoadQueue final {
public:
    using Finalize = std::function<bool()>;
    using Work = std::function<Finalize()>;
    using OnError = std::function<void(const std::string&)>;
    struct Prepared final {
        Finalize finalize;
        std::shared_ptr<rhi::IUploadTicket> ticket;
        std::uint64_t bytes{0};
    };
    using Prepare = std::function<Prepared()>;

    AsyncLoadQueue();
    ~AsyncLoadQueue();
    AsyncLoadQueue(const AsyncLoadQueue&) = delete;
    AsyncLoadQueue& operator=(const AsyncLoadQueue&) = delete;

    void start(core::JobSystem* jobs);
    void setAsyncEnabled(bool enabled);
    void setUploadBudget(std::size_t maximumUploads, double milliseconds);
    // 0 restores the workers-dependent default; explicit values are bounded to 16.
    void setInFlightLimit(std::size_t maximumAssets);
    void submit(Work work, OnError onError);
    // Worker can submit transfer commands, but only the owner publishes assets.
    void submitPrepared(Prepare work, OnError onError);
    void pump();
    void stop();
    [[nodiscard]] AssetLoadingStats stats() const;

private:
    struct State;
    struct Completion;
    struct Pending final {
        Prepare work;
        std::shared_ptr<Completion> completion;
    };
    std::shared_ptr<State> m_state;
    core::JobSystem* m_jobs{nullptr};
    std::vector<core::JobSystem::Handle> m_handles;
    std::deque<Pending> m_waiting;
    std::deque<std::shared_ptr<Completion>> m_completions;
    std::thread::id m_owner;
    std::size_t m_maximumUploads{1};
    std::size_t m_maximumInFlight{0};
    double m_uploadBudgetMilliseconds{2.0};
    bool m_asyncEnabled{true};
    bool m_stopped{false};

    void requireOwner() const;
    [[nodiscard]] std::size_t inFlightLimit() const;
    void dispatchPending();
    void finalize(const std::shared_ptr<Completion>& completion);
};

} // namespace engine::resources
