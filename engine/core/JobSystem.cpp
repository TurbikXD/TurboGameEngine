#include "engine/core/JobSystem.h"

#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <TaskScheduler.h>
#include "engine/core/Profiling.h"

namespace engine::core {
namespace {
enki::TaskPriority nativePriority(const JobSystem::Priority priority) {
    switch (priority) {
    case JobSystem::Priority::High: return enki::TASK_PRIORITY_HIGH;
    case JobSystem::Priority::Normal: return enki::TASK_PRIORITY_MED;
    case JobSystem::Priority::Low: return enki::TASK_PRIORITY_LOW;
    }
    throw std::invalid_argument("Invalid job priority");
}

void nameWorker(const std::uint32_t index) {
    const std::string name = "Job Worker " + std::to_string(index);
    ENGINE_PROFILE_THREAD_NAME(name.c_str());
}
} // namespace

struct JobSystem::JobState final : enki::ITaskSet {
    Impl* owner{};
    std::shared_ptr<const int> identity;
    std::function<void(std::size_t, std::size_t)> function;
    std::vector<std::shared_ptr<JobState>> prerequisites;
    std::vector<std::weak_ptr<JobState>> continuations;
    std::atomic_size_t prerequisitesLeft{1U}; // Registration gate prevents early scheduling.
    std::atomic_size_t itemsLeft{1U};
    std::atomic_bool started{false};
    std::atomic_bool bodyDone{false};
    std::mutex completionMutex;
    std::exception_ptr exception;

    void ExecuteRange(enki::TaskSetPartition range, std::uint32_t threadNumber) override;
    void releasePrerequisite();
};

struct JobSystem::Impl final {
    struct ExecutionScope final {
        ExecutionScope* previousScope;
        Impl* owner;
        JobState* task;
        Impl* previousOwner;
        JobState* previousTask;
        ExecutionScope(Impl* currentOwner, JobState* currentTask)
            : previousScope(executionTop), owner(currentOwner), task(currentTask),
              previousOwner(executingOwner), previousTask(executingTask) {
            executionTop = this;
            executingOwner = currentOwner;
            executingTask = currentTask;
        }
        ~ExecutionScope() {
            executionTop = previousScope;
            executingOwner = previousOwner;
            executingTask = previousTask;
        }
    };
    static thread_local ExecutionScope* executionTop;
    static thread_local Impl* executingOwner;
    static thread_local JobState* executingTask;

    explicit Impl(const bool jobsEnabled, std::size_t requestedWorkers)
        : parallel(jobsEnabled), creator(std::this_thread::get_id()) {
        if (!parallel) return;
        if (requestedWorkers == 0U) {
            const auto hardware = std::thread::hardware_concurrency();
            requestedWorkers = std::min<std::size_t>(hardware > 1U ? hardware - 1U : 1U, 4U);
        }
        if (requestedWorkers >= std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Too many job workers");
        }
        workers = requestedWorkers;
        enki::TaskSchedulerConfig config;
        config.numTaskThreadsToCreate = static_cast<std::uint32_t>(workers);
        config.profilerCallbacks.threadStart = nameWorker;
        scheduler.Initialize(config);
    }

    void requireThread() const {
        if (std::this_thread::get_id() != creator && executingOwner != this) {
            throw std::logic_error("JobSystem API requires its creator thread or one of its jobs");
        }
    }

    void requireOwner() const {
        if (std::this_thread::get_id() != creator || executingOwner == this) {
            throw std::logic_error("JobSystem drain/shutdown requires the idle creator thread");
        }
    }

    void validate(const std::shared_ptr<JobState>& state) const {
        if (state && state->identity != identity) {
            throw std::invalid_argument("Job handle belongs to another JobSystem");
        }
    }

    static bool complete(const JobState& state) {
        return state.bodyDone.load(std::memory_order_acquire) && state.GetIsComplete();
    }

    void schedule(JobState* state) {
        if (parallel) scheduler.AddTaskSetToPipe(state);
        else state->ExecuteRange({0U, state->m_SetSize}, 0U);
    }

    std::shared_ptr<JobState> submit(
        std::function<void(std::size_t, std::size_t)> function, const std::size_t count,
        const std::size_t grain, const Priority priority,
        const std::vector<Handle>& dependencies = {}) {
        requireThread();
        if (stopped || (draining && executingOwner != this)) {
            throw std::logic_error("JobSystem is shutting down");
        }
        if (count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("Job range exceeds enkiTS uint32 range capacity");
        }
        auto state = std::make_shared<JobState>();
        state->owner = this;
        state->identity = identity;
        state->function = std::move(function);
        state->m_SetSize = static_cast<std::uint32_t>(count);
        state->m_MinRange = static_cast<std::uint32_t>(std::min(count, std::max<std::size_t>(grain, 1U)));
        state->m_Priority = nativePriority(priority);
        state->itemsLeft.store(count, std::memory_order_relaxed);
        state->prerequisites.reserve(dependencies.size());
        for (const auto& dependency : dependencies) {
            validate(dependency.m_state);
            if (dependency.m_state && std::find(state->prerequisites.begin(), state->prerequisites.end(),
                    dependency.m_state) == state->prerequisites.end()) {
                state->prerequisites.push_back(dependency.m_state);
            }
        }

        // Reserve all edges before publishing the job. A failed allocation must
        // not leave an unschedulable task in the registry. Lock ordering allows
        // concurrent worker submissions with overlapping prerequisites.
        std::vector<JobState*> lockOrder;
        lockOrder.reserve(state->prerequisites.size());
        for (const auto& dependency : state->prerequisites) lockOrder.push_back(dependency.get());
        std::sort(lockOrder.begin(), lockOrder.end(), std::less<JobState*>{});
        std::vector<std::unique_lock<std::mutex>> dependencyLocks;
        dependencyLocks.reserve(lockOrder.size());
        for (auto* dependency : lockOrder) {
            dependencyLocks.emplace_back(dependency->completionMutex);
            if (!dependency->bodyDone.load(std::memory_order_relaxed)) {
                dependency->continuations.reserve(dependency->continuations.size() + 1U);
            }
        }

        {
            std::lock_guard lock(activeMutex);
            std::erase_if(active, [](const auto& job) { return complete(*job); });
            active.push_back(state); // Keep fire-and-forget tasks alive until enkiTS releases them.
        }
        submitted.fetch_add(1U, std::memory_order_relaxed);
        queued.fetch_add(1U, std::memory_order_relaxed);
        state->prerequisitesLeft.store(state->prerequisites.size() + 1U, std::memory_order_relaxed);
        // Native enkiTS dependency edges must be attached before their roots are
        // launched. Handles are already running, so use synchronized continuations
        // here and enqueue only after every predecessor's body is complete.
        for (const auto& dependency : state->prerequisites) {
            if (dependency->bodyDone.load(std::memory_order_relaxed)) {
                state->prerequisitesLeft.fetch_sub(1U, std::memory_order_relaxed);
            } else {
                dependency->continuations.push_back(state);
            }
        }
        dependencyLocks.clear();
        state->releasePrerequisite();
        return state;
    }

    void await(const std::shared_ptr<JobState>& state) {
        if (!state || complete(*state)) return;
        for (auto* scope = executionTop; scope; scope = scope->previousScope) {
            if (scope->owner == this && scope->task == state.get()) {
                throw std::logic_error("A job cannot wait for itself or an executing ancestor");
            }
        }
        // Wait prerequisites first: an unqueued continuation has native count 0.
        // Each wait keeps its own priority threshold, avoiding unrelated low-priority IO.
        for (const auto& prerequisite : state->prerequisites) await(prerequisite);
        if (parallel) scheduler.WaitforTask(state.get(), state->m_Priority);
    }

    bool parallel{};
    std::size_t workers{};
    std::thread::id creator;
    std::shared_ptr<const int> identity = std::make_shared<const int>(0);
    enki::TaskScheduler scheduler;
    std::atomic_bool draining{false};
    std::atomic_bool stopped{false};
    std::mutex activeMutex;
    std::vector<std::shared_ptr<JobState>> active;
    std::atomic_uint64_t submitted{0U}, completed{0U}, ranges{0U};
    std::atomic_size_t queued{0U}, running{0U};
};

thread_local JobSystem::Impl* JobSystem::Impl::executingOwner = nullptr;
thread_local JobSystem::JobState* JobSystem::Impl::executingTask = nullptr;
thread_local JobSystem::Impl::ExecutionScope* JobSystem::Impl::executionTop = nullptr;

void JobSystem::JobState::releasePrerequisite() {
    if (prerequisitesLeft.fetch_sub(1U, std::memory_order_acq_rel) == 1U) owner->schedule(this);
}

void JobSystem::JobState::ExecuteRange(const enki::TaskSetPartition range, const std::uint32_t) {
    ENGINE_PROFILE_ZONE("Job Execute");
    Impl::ExecutionScope execution(owner, this);
    if (!started.exchange(true, std::memory_order_relaxed)) owner->queued.fetch_sub(1U, std::memory_order_relaxed);
    owner->running.fetch_add(1U, std::memory_order_relaxed);
    owner->ranges.fetch_add(1U, std::memory_order_relaxed);
    try {
        for (const auto& dependency : prerequisites) {
            std::lock_guard lock(dependency->completionMutex);
            if (dependency->exception) std::rethrow_exception(dependency->exception);
        }
        function(range.start, range.end);
    } catch (...) {
        std::lock_guard lock(completionMutex);
        if (!exception) exception = std::current_exception();
    }
    owner->running.fetch_sub(1U, std::memory_order_relaxed);
    if (itemsLeft.fetch_sub(range.end - range.start, std::memory_order_acq_rel) == range.end - range.start) {
        std::vector<std::weak_ptr<JobState>> ready;
        {
            std::lock_guard lock(completionMutex);
            function = {};
            ready.swap(continuations);
            bodyDone.store(true, std::memory_order_release);
        }
        owner->completed.fetch_add(1U, std::memory_order_relaxed);
        for (const auto& weak : ready) {
            if (const auto child = weak.lock()) child->releasePrerequisite();
        }
    }
}

JobSystem::Handle::Handle(std::shared_ptr<JobState> state) : m_state(std::move(state)) {}
bool JobSystem::Handle::valid() const { return m_state != nullptr; }
JobSystem::JobSystem(const bool enabled, const std::size_t workerCount)
    : m_impl(std::make_unique<Impl>(enabled, workerCount)) {}
JobSystem::~JobSystem() { shutdown(); }
bool JobSystem::enabled() const { return m_impl->parallel && !m_impl->stopped; }
std::size_t JobSystem::workerCount() const { return enabled() ? m_impl->workers : 0U; }

JobSystem::Handle JobSystem::dispatch(std::function<void()> job, const Priority priority) {
    return dispatchAfter({}, std::move(job), priority);
}

JobSystem::Handle JobSystem::dispatchAfter(const std::vector<Handle>& prerequisites,
                                          std::function<void()> job, const Priority priority) {
    ENGINE_PROFILE_ZONE("Job Dispatch");
    if (!job) throw std::invalid_argument("JobSystem::dispatch requires a callable");
    return Handle(m_impl->submit([function = std::move(job)](std::size_t, std::size_t) { function(); },
                                 1U, 1U, priority, prerequisites));
}

bool JobSystem::isComplete(const Handle& handle) const {
    m_impl->validate(handle.m_state);
    return !handle.m_state || Impl::complete(*handle.m_state);
}

void JobSystem::wait(const Handle& handle) const {
    ENGINE_PROFILE_ZONE("Job Wait");
    m_impl->requireThread();
    m_impl->validate(handle.m_state);
    m_impl->await(handle.m_state);
    if (handle.m_state && handle.m_state->exception) std::rethrow_exception(handle.m_state->exception);
}

void JobSystem::waitAll() {
    ENGINE_PROFILE_ZONE("Job Wait All");
    m_impl->requireOwner();
    if (m_impl->stopped) return;
    if (m_impl->parallel) m_impl->scheduler.WaitforAll();
    std::lock_guard lock(m_impl->activeMutex);
    m_impl->active.clear();
}

void JobSystem::shutdown() {
    if (m_impl->stopped) return;
    m_impl->requireOwner();
    ENGINE_PROFILE_ZONE("Job Shutdown");
    m_impl->draining.store(true, std::memory_order_release);
    if (m_impl->parallel) m_impl->scheduler.WaitforAllAndShutdown();
    m_impl->stopped.store(true, std::memory_order_release);
    std::lock_guard lock(m_impl->activeMutex);
    m_impl->active.clear();
}

JobSystem::Stats JobSystem::stats() const {
    Stats result;
    result.completed = m_impl->completed.load(std::memory_order_relaxed);
    result.submitted = m_impl->submitted.load(std::memory_order_relaxed);
    result.executedRanges = m_impl->ranges.load(std::memory_order_relaxed);
    result.pending = static_cast<std::size_t>(result.submitted >= result.completed ? result.submitted - result.completed : 0U);
    result.queueDepth = m_impl->queued.load(std::memory_order_relaxed);
    result.running = m_impl->running.load(std::memory_order_relaxed);
    return result;
}

void JobSystem::publishStats() const {
    const auto snapshot = stats();
    ENGINE_PROFILE_PLOT("Jobs Pending", snapshot.pending);
    ENGINE_PROFILE_PLOT("Jobs Queue Depth", snapshot.queueDepth);
    ENGINE_PROFILE_PLOT("Jobs Running Ranges", snapshot.running);
    ENGINE_PROFILE_PLOT("Jobs Completed", snapshot.completed);
    ENGINE_PROFILE_PLOT("Jobs Executed Ranges", snapshot.executedRanges);
}

void JobSystem::runParallelFor(const std::size_t count, const std::size_t grain,
                             std::function<void(std::size_t, std::size_t)> function) {
    ENGINE_PROFILE_ZONE("Job Parallel For");
    const Handle handle(m_impl->submit(std::move(function), count, grain, Priority::Normal));
    wait(handle);
}

} // namespace engine::core
