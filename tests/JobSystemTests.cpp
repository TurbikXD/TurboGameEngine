#include <atomic>
#include <cstdlib>
#include <iostream>
#include <future>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/core/JobSystem.h"

namespace {

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::cerr << "Test failure: " << message << '\n';
    return false;
}

bool runParallelForTest() {
    engine::core::JobSystem jobs(true, 3U);
    std::vector<std::atomic_uint32_t> visits(4096U);
    for (auto& visit : visits) {
        visit.store(0U);
    }

    jobs.parallelFor(visits.size(), 64U, [&visits](const std::size_t begin, const std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            visits[index].fetch_add(1U, std::memory_order_relaxed);
        }
    });

    for (const auto& visit : visits) {
        if (!expect(visit.load(std::memory_order_relaxed) == 1U, "parallelFor must visit every item exactly once")) {
            return false;
        }
    }
    return expect(jobs.workerCount() == 3U, "explicit worker count was not respected");
}

bool runDispatchAndWaitTest() {
    engine::core::JobSystem jobs(true, 2U);
    std::atomic_int sum{0};
    std::vector<engine::core::JobSystem::Handle> handles;
    for (int value = 1; value <= 100; ++value) {
        handles.push_back(jobs.dispatch([&sum, value]() { sum.fetch_add(value, std::memory_order_relaxed); }));
    }
    for (const auto& handle : handles) {
        jobs.wait(handle);
    }
    return expect(sum.load(std::memory_order_relaxed) == 5050, "dispatch/wait produced the wrong result");
}

bool runDisabledAndExceptionTest() {
    engine::core::JobSystem jobs(false);
    const std::thread::id caller = std::this_thread::get_id();
    std::thread::id executor;
    const auto handle = jobs.dispatch([&executor]() { executor = std::this_thread::get_id(); });
    jobs.wait(handle);
    if (!expect(executor == caller, "disabled job system must execute synchronously")) {
        return false;
    }

    const auto throwingJob = jobs.dispatch([]() { throw std::runtime_error("expected"); });
    try {
        jobs.wait(throwingJob);
    } catch (const std::runtime_error&) {
        return true;
    }
    return expect(false, "worker exception was not propagated by wait");
}

bool runNestedAndDependencyTest() {
    using Jobs = engine::core::JobSystem;
    Jobs jobs(true, 1U);
    std::atomic_int visits{0};
    const auto parent = jobs.dispatch([&]() {
        jobs.parallelFor(257U, 16U, [&](std::size_t begin, std::size_t end) {
            const auto child = jobs.dispatch([&visits, begin, end]() {
                visits.fetch_add(static_cast<int>(end - begin), std::memory_order_relaxed);
            });
            jobs.wait(child);
        });
    });
    jobs.wait(parent);
    if (!expect(visits == 257, "nested dispatch/parallelFor failed with one worker")) return false;

    int valueA = 0;
    int valueB = 0;
    int result = 0;
    const auto a = jobs.dispatch([&]() { valueA = 20; }, Jobs::Priority::Low);
    const auto b = jobs.dispatch([&]() { valueB = 22; });
    const auto joined = jobs.dispatchAfter({a, b, a, {}}, [&]() { result = valueA + valueB; }, Jobs::Priority::High);
    const auto final = jobs.dispatchAfter({joined}, [&]() { result *= 2; });
    jobs.wait(final);
    if (!expect(result == 84 && jobs.isComplete(final), "dependency fan-in/order failed")) return false;

    bool dependentRan = false;
    const auto failed = jobs.dispatch([]() { throw std::runtime_error("dependency failure"); });
    const auto skipped = jobs.dispatchAfter({failed}, [&]() { dependentRan = true; });
    try {
        jobs.wait(skipped);
    } catch (const std::runtime_error&) {
        return expect(!dependentRan && jobs.isComplete(skipped), "failed dependency did not skip its continuation");
    }
    return expect(false, "dependency exception was not propagated");
}

bool runExceptionAndBoundaryTest() {
    using Jobs = engine::core::JobSystem;
    Jobs jobs(true, 2U);
    bool emptyCalled = false;
    jobs.parallelFor(0U, 0U, [&](std::size_t, std::size_t) { emptyCalled = true; });
    if (!expect(!emptyCalled && jobs.isComplete({}), "empty work must be a no-op")) return false;
    try {
        jobs.parallelFor(513U, 17U, [](std::size_t begin, std::size_t) {
            if (begin == 0U) throw std::runtime_error("range failure");
        });
        return expect(false, "parallelFor did not rethrow range exception");
    } catch (const std::runtime_error&) {
        if (!expect(jobs.stats().running == 0U && jobs.stats().pending == 0U,
                    "parallelFor returned before every range finished")) return false;
    }
    try {
        jobs.dispatch({});
        return expect(false, "empty callable must be rejected");
    } catch (const std::invalid_argument&) {}
    Jobs other(false);
    const auto foreign = other.dispatch([]() {});
    try {
        jobs.wait(foreign);
        return expect(false, "foreign handle must be rejected");
    } catch (const std::invalid_argument&) {}
    std::atomic_bool rejected{false};
    std::thread external([&]() {
        try { jobs.dispatch([]() {}); }
        catch (const std::logic_error&) { rejected = true; }
    });
    external.join();
    return expect(rejected, "unregistered external submission must be rejected");
}

bool runPriorityIsolationTest() {
    using Jobs = engine::core::JobSystem;
    Jobs jobs(true, 1U);
    const auto creator = std::this_thread::get_id();
    std::promise<void> workerStarted;
    auto started = workerStarted.get_future();
    std::latch releaseWorker(1);
    const auto blocker = jobs.dispatch([&]() { workerStarted.set_value(); releaseWorker.wait(); }, Jobs::Priority::Low);
    started.wait();
    std::atomic_bool normalWait{true};
    std::atomic_bool ranLowDuringNormalWait{false};
    const auto low = jobs.dispatch([&]() {
        if (normalWait && std::this_thread::get_id() == creator) ranLowDuringNormalWait = true;
    }, Jobs::Priority::Low);
    bool normalRan = false;
    const auto normal = jobs.dispatch([&]() { normalRan = true; });
    jobs.wait(normal);
    normalWait = false;
    releaseWorker.count_down();
    jobs.wait(blocker);
    jobs.wait(low);
    return expect(normalRan && !ranLowDuringNormalWait, "normal wait executed unrelated low-priority IO");
}

bool runShutdownAndStressTest() {
    using Jobs = engine::core::JobSystem;
    std::atomic_int total{0};
    {
        Jobs jobs(true, 2U);
        for (int i = 0; i < 500; ++i) {
            const auto parent = jobs.dispatch([&]() { total.fetch_add(1, std::memory_order_relaxed); });
            jobs.dispatchAfter({parent}, [&]() { total.fetch_add(1, std::memory_order_relaxed); });
        }
        jobs.dispatch([&]() {
            const auto child = jobs.dispatch([&]() { total.fetch_add(1, std::memory_order_relaxed); });
            jobs.wait(child);
        });
        jobs.shutdown();
        jobs.shutdown();
        const auto stats = jobs.stats();
        if (!expect(total == 1001 && stats.pending == 0U && stats.queueDepth == 0U && stats.running == 0U,
                    "shutdown did not drain in-flight and nested jobs")) return false;
        try {
            jobs.dispatch([]() {});
            return expect(false, "dispatch after shutdown must be rejected");
        } catch (const std::logic_error&) {}
    }
    {
        Jobs jobs(true, 2U);
        for (int i = 0; i < 100; ++i) jobs.dispatch([&]() { total.fetch_add(1, std::memory_order_relaxed); });
    }
    return expect(total == 1101, "destructor lost fire-and-forget jobs");
}

} // namespace

int main() {
    if (!runParallelForTest() || !runDispatchAndWaitTest() || !runDisabledAndExceptionTest()
        || !runNestedAndDependencyTest() || !runExceptionAndBoundaryTest()
        || !runPriorityIsolationTest() || !runShutdownAndStressTest()) {
        return EXIT_FAILURE;
    }
    std::cout << "JobSystem tests passed\n";
    return EXIT_SUCCESS;
}
