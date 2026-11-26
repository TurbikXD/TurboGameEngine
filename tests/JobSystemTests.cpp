#include <atomic>
#include <cstdlib>
#include <iostream>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/core/JobSystem.h"
#include "engine/core/LabOptions.h"

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

bool runWorkerCountOptionTest() {
    using engine::core::LabOptions;
    if (!expect(LabOptions::boundedCount("4", 256U) == 4U &&
                LabOptions::boundedCount("24", 256U) == 24U &&
                LabOptions::boundedCount("256", 256U) == 256U,
                "Worker options did not preserve explicit counts")) { return false; }
    for (const auto* invalid : {"", "0", "-1", "+24", "24.5", "24junk", " 24", "257", "999999999999999999999"}) {
        if (!expect(LabOptions::boundedCount(invalid, 256U) == 0U, "Invalid worker option was accepted")) { return false; }
    }
    engine::core::JobSystem jobs(true, 24U);
    std::atomic_size_t visits{0U};
    jobs.parallelFor(4096U, 64U, [&visits](const std::size_t begin, const std::size_t end) {
        visits.fetch_add(end - begin, std::memory_order_relaxed);
    });
    return expect(jobs.workerCount() == 24U && visits == 4096U, "24-worker scheduler lost ranges");
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

bool runAncestorDrainRejectionTest() {
    using Jobs = engine::core::JobSystem;
    Jobs outer(false);
    Jobs inner(false);
    bool waitRejected = false;
    bool shutdownRejected = false;
    const auto parent = outer.dispatch([&] {
        const auto child = inner.dispatch([&] {
            try { outer.waitAll(); }
            catch (const std::logic_error&) { waitRejected = true; }
            try { outer.shutdown(); }
            catch (const std::logic_error&) { shutdownRejected = true; }
        });
        inner.wait(child);
    });
    outer.wait(parent);
    if (!expect(waitRejected && shutdownRejected, "A nested foreign task drained its executing ancestor JobSystem")) return false;
    bool usable = false;
    outer.wait(outer.dispatch([&] { usable = true; }));
    outer.waitAll();
    return expect(usable && outer.stats().pending == 0U, "Ancestor drain rejection left the JobSystem unusable");
}

bool runCapturedCleanupContinuationTest() {
    using Jobs = engine::core::JobSystem;
    Jobs jobs(true, 1U);
    Jobs::Handle parent;
    std::atomic_size_t released{0}, cleaned{0};
    std::atomic_bool dispatchFailed{false};
    struct Payload final {
        Jobs& jobs;
        const Jobs::Handle& parent;
        std::atomic_size_t& released;
        std::atomic_size_t& cleaned;
        std::atomic_bool& dispatchFailed;
        ~Payload() {
            released.fetch_add(1U, std::memory_order_relaxed);
            try {
                jobs.dispatchAfter({parent}, [&counter = cleaned] { counter.fetch_add(1U, std::memory_order_relaxed); });
            } catch (...) {
                dispatchFailed.store(true, std::memory_order_relaxed);
            }
        }
    };
    auto payload = std::make_shared<Payload>(jobs, parent, released, cleaned, dispatchFailed);
    std::latch finishParent(1);
    parent = jobs.dispatch([hold = payload, &finishParent] { finishParent.wait(); });
    payload.reset();
    finishParent.count_down();
    jobs.wait(parent);
    jobs.waitAll();
    const auto stats = jobs.stats();
    return expect(released == 1U && cleaned == 1U && !dispatchFailed && stats.submitted == 2U &&
                  stats.completed == 2U && stats.pending == 0U && stats.queueDepth == 0U,
                  "Captured task cleanup could not enqueue a continuation depending on its parent");
}

bool runNestedWorkerSchedulerRejectionTest() {
    using Jobs = engine::core::JobSystem;
    Jobs jobs(true, 1U);
    std::promise<bool> checked;
    auto result = checked.get_future();
    const auto parent = jobs.dispatch([&] {
        try {
            bool rejected = false;
            try { Jobs nested(true, 1U); }
            catch (const std::logic_error&) { rejected = true; }
            if (!rejected) {
                checked.set_value(false);
                return;
            }
            std::atomic_bool childRan{false};
            const auto child = jobs.dispatch([&] { childRan = true; });
            jobs.wait(child);
            checked.set_value(childRan.load(std::memory_order_relaxed));
        } catch (...) {
            checked.set_exception(std::current_exception());
        }
    });
    bool valid = false;
    try { valid = result.get(); }
    catch (...) { valid = false; }
    jobs.wait(parent);
    return expect(valid && jobs.stats().pending == 0U,
        "Worker-created scheduler replaced its outer native worker registration or damaged nested dispatch");
}

bool runNestedDisabledWorkerAdmissionTest() {
    using Jobs = engine::core::JobSystem;
    for (const bool shutdown : {false, true}) {
        Jobs jobs(true, 1U);
        Jobs::Handle parent;
        std::promise<void> workerStarted;
        auto started = workerStarted.get_future();
        std::promise<bool> checked;
        auto result = checked.get_future();
        std::latch beginNested(1);
        parent = jobs.dispatch([&] {
            workerStarted.set_value();
            beginNested.wait();
            try {
                Jobs nested(false);
                bool valid = false;
                const auto callback = nested.dispatch([&] {
                    std::atomic_bool childRan{false};
                    const auto child = jobs.dispatch([&] { childRan = true; });
                    jobs.wait(child);
                    bool ancestorRejected = false;
                    try { jobs.wait(parent); }
                    catch (const std::logic_error&) { ancestorRejected = true; }
                    std::atomic_size_t visited{0};
                    jobs.parallelFor(23U, 3U, [&](const std::size_t begin, const std::size_t end) {
                        visited.fetch_add(end - begin, std::memory_order_relaxed);
                    });
                    valid = childRan && ancestorRejected && visited == 23U;
                });
                nested.wait(callback);
                checked.set_value(valid);
            } catch (...) {
                checked.set_exception(std::current_exception());
            }
        });
        started.wait();
        if (shutdown) {
            jobs.dispatch([&] { beginNested.count_down(); });
            jobs.shutdown();
        } else {
            beginNested.count_down();
        }
        bool valid = false;
        try { valid = result.get(); }
        catch (...) { valid = false; }
        jobs.wait(parent);
        jobs.waitAll();
        if (!expect(valid && jobs.stats().pending == 0U,
            "Nested disabled callback lost outer worker admission or could not submit while shutdown drained")) return false;
    }
    return true;
}

}

int main() {
    if (!runWorkerCountOptionTest() || !runParallelForTest() || !runDispatchAndWaitTest() || !runDisabledAndExceptionTest()
        || !runNestedAndDependencyTest() || !runExceptionAndBoundaryTest()
        || !runPriorityIsolationTest() || !runShutdownAndStressTest()
        || !runAncestorDrainRejectionTest() || !runCapturedCleanupContinuationTest()
        || !runNestedWorkerSchedulerRejectionTest() || !runNestedDisabledWorkerAdmissionTest()) {
        return EXIT_FAILURE;
    }
    std::cout << "JobSystem tests passed\n";
    return EXIT_SUCCESS;
}
