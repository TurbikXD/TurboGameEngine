#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace engine::core {

class JobSystem final {
private:
    struct JobState;

public:
    enum class Priority { High, Normal, Low };

    struct Stats final {
        std::uint64_t submitted{};
        std::uint64_t completed{};
        std::uint64_t executedRanges{};
        std::size_t pending{};       // Submitted task sets not yet complete.
        std::size_t queueDepth{};    // Task sets whose first range has not started.
        std::size_t running{};       // Executing ranges, including suspended nested callers.
    };

    class Handle final {
    public:
        Handle() = default;
        [[nodiscard]] bool valid() const;

    private:
        friend class JobSystem;
        explicit Handle(std::shared_ptr<JobState> state);

        std::shared_ptr<JobState> m_state;
    };

    explicit JobSystem(bool enabled = true, std::size_t workerCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    [[nodiscard]] bool enabled() const;
    [[nodiscard]] std::size_t workerCount() const;

    // Submission and cooperative waits belong on the creator thread or a task of
    // this scheduler. Callbacks must not mutate ECS structure or use GPU APIs.
    // Wait only for child/independent work; do not create circular wait graphs.
    Handle dispatch(std::function<void()> job, Priority priority = Priority::Normal);
    Handle dispatchAfter(const std::vector<Handle>& prerequisites, std::function<void()> job,
                         Priority priority = Priority::Normal);
    [[nodiscard]] bool isComplete(const Handle& handle) const;
    void wait(const Handle& handle) const;
    // Owner thread only. Drain outstanding work, including children spawned by it.
    // Job exceptions are reported by wait(handle), not by these drain operations.
    void waitAll();
    void shutdown();
    [[nodiscard]] Stats stats() const;
    void publishStats() const;

    template <class RangeFunction>
    void parallelFor(std::size_t itemCount, std::size_t minimumItemsPerJob, RangeFunction&& function);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    void runParallelFor(std::size_t count, std::size_t grain,
                        std::function<void(std::size_t, std::size_t)> function);
};

template <class RangeFunction>
void JobSystem::parallelFor(
    const std::size_t itemCount,
    const std::size_t minimumItemsPerJob,
    RangeFunction&& function) {
    if (itemCount == 0U) {
        return;
    }

    using Function = std::decay_t<RangeFunction>;
    auto sharedFunction = std::make_shared<Function>(std::forward<RangeFunction>(function));
    runParallelFor(itemCount, minimumItemsPerJob,
        [sharedFunction](std::size_t begin, std::size_t end) { (*sharedFunction)(begin, end); });
}

} // namespace engine::core
