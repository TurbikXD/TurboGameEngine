#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/resources/async_load_queue.h"
#include "engine/resources/loaders.h"

namespace {
using engine::core::JobSystem;
using engine::resources::AsyncLoadQueue;

void expect(const bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void waitForCpu(AsyncLoadQueue& queue) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (queue.stats().pendingCpu != 0U) {
        expect(std::chrono::steady_clock::now() < deadline, "CPU asset jobs did not finish within 5 seconds");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void testPumpThreadAndCountBudget() {
    JobSystem jobs(true, 2U);
    AsyncLoadQueue queue;
    queue.start(&jobs);
    queue.setUploadBudget(1U, 1000.0);
    const auto mainThread = std::this_thread::get_id();
    std::atomic_size_t workerRuns{0};
    std::size_t uploads = 0;
    for (int index = 0; index < 4; ++index) {
        queue.submit([&]() -> AsyncLoadQueue::Finalize {
            if (std::this_thread::get_id() != mainThread) {
                workerRuns.fetch_add(1U);
            }
            return [&]() {
                expect(std::this_thread::get_id() == mainThread, "GPU finalize escaped main thread");
                ++uploads;
                return true;
            };
        }, {});
    }
    waitForCpu(queue);
    expect(uploads == 0U, "Worker ran a GPU finalization");
    expect(workerRuns.load() == 4U, "Asynchronous work did not execute on workers");
    for (std::size_t index = 0; index < 4U; ++index) {
        queue.pump();
        expect(uploads == index + 1U, "Count budget must finalize one resource per frame");
    }
    const auto stats = queue.stats();
    expect(stats.loaded == 4U && stats.failed == 0U && stats.pendingUploads == 0U,
           "Successful async loading counters are inconsistent");
}

void testTimeBudget() {
    JobSystem jobs(true, 2U);
    AsyncLoadQueue queue;
    queue.start(&jobs);
    queue.setUploadBudget(10U, 0.1);
    for (int index = 0; index < 3; ++index) {
        queue.submit([]() -> AsyncLoadQueue::Finalize {
            return []() {
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                return true;
            };
        }, {});
    }
    waitForCpu(queue);
    queue.pump();
    expect(queue.stats().uploadedLastFrame == 1U && queue.stats().pendingUploads == 2U,
           "Time budget must stop before starting the second upload");
}

void testExceptionsAndMissingInput() {
    JobSystem jobs(true, 2U);
    AsyncLoadQueue queue;
    queue.start(&jobs);
    queue.setUploadBudget(8U, 1000.0);
    std::size_t errors = 0;
    const auto mainThread = std::this_thread::get_id();
    const auto onError = [&](const std::string& message) {
        expect(!message.empty(), "Failure lost its diagnostic");
        expect(std::this_thread::get_id() == mainThread, "Error callback escaped main thread");
        ++errors;
    };
    queue.submit([]() -> AsyncLoadQueue::Finalize { throw std::runtime_error("decoder failed"); }, onError);
    queue.submit([]() -> AsyncLoadQueue::Finalize {
        return []() -> bool { throw std::runtime_error("driver failed"); };
    }, onError);
    queue.submit([]() -> AsyncLoadQueue::Finalize { throw 42; }, onError);
    queue.submit([]() -> AsyncLoadQueue::Finalize {
        engine::resources::TextureData texture;
        engine::resources::MeshData mesh;
        std::string textureError;
        std::string meshError;
        const bool textureLoaded = engine::resources::loadTextureDataRgba8(
            "__lab1_nonexistent_resource__/missing.png", texture, &textureError);
        const bool meshLoaded = engine::resources::loadMeshData(
            "__lab1_nonexistent_resource__/missing.obj", mesh, &meshError);
        if (textureLoaded || meshLoaded || textureError.empty() || meshError.empty()) {
            throw std::runtime_error("missing-resource diagnostics broken");
        }
        return []() { return false; };
    }, onError);
    waitForCpu(queue);
    queue.pump();
    expect(queue.stats().failed == 4U && errors == 3U, "Exception/missing-input accounting failed");
}

void testShutdownWithLiveJobsAndRestart() {
    JobSystem jobs(true, 2U);
    AsyncLoadQueue queue;
    queue.start(&jobs);
    std::atomic_size_t decodes{0};
    std::size_t uploads = 0;
    std::size_t cancellations = 0;
    for (int index = 0; index < 24; ++index) {
        queue.submit([&]() -> AsyncLoadQueue::Finalize {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            decodes.fetch_add(1U);
            return [&]() { ++uploads; return true; };
        }, [&](const std::string& error) {
            if (error.find("cancelled") != std::string::npos) {
                ++cancellations;
            }
        });
    }
    queue.stop();
    expect(decodes.load() >= 1U && decodes.load() <= 4U && uploads == 0U && cancellations == 24U,
           "Shutdown did not join running jobs, cancel backlog and cancel pending GPU work");
    expect(queue.stats().pendingCpu == 0U && queue.stats().pendingUploads == 0U,
           "Shutdown left live resource jobs");
    queue.stop();
    queue.start(&jobs);
    expect(queue.stats().submitted == 0U, "Restart did not reset counters");
}

void testSynchronousBaseline() {
    JobSystem jobs(true, 2U);
    AsyncLoadQueue queue;
    queue.start(&jobs);
    queue.setAsyncEnabled(false);
    const auto mainThread = std::this_thread::get_id();
    bool done = false;
    queue.submit([&]() -> AsyncLoadQueue::Finalize {
        expect(std::this_thread::get_id() == mainThread, "Synchronous baseline did not read on main thread");
        return [&]() { done = true; return true; };
    }, {});
    expect(done && queue.stats().loaded == 1U && !queue.stats().asyncEnabled,
           "Synchronous baseline did not finalize immediately");
}

void testLongSessionWithMixedOutcomes() {
    JobSystem jobs(true, 3U);
    AsyncLoadQueue queue;
    queue.start(&jobs);
    queue.setUploadBudget(8U, 10.0);
    constexpr std::size_t count = 1024U;
    std::vector<std::atomic_uint32_t> executions(count);
    std::size_t expectedFailures = 0;
    for (std::size_t batch = 0; batch < 16U; ++batch) {
        for (std::size_t index = batch * 64U; index < (batch + 1U) * 64U; ++index) {
            if (index % 7U == 0U || index % 11U == 0U) {
                ++expectedFailures;
            }
            queue.submit([&, index]() -> AsyncLoadQueue::Finalize {
                executions[index].fetch_add(1U);
                if (index % 11U == 0U) {
                    throw std::runtime_error("expected batch decoder error");
                }
                return [index]() { return index % 7U != 0U; };
            }, {});
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (queue.stats().pendingCpu != 0U || queue.stats().pendingUploads != 0U) {
            expect(std::chrono::steady_clock::now() < deadline, "Long session stopped making progress");
            queue.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    for (const auto& executionsForAsset : executions) {
        expect(executionsForAsset.load() == 1U, "Asset job ran more or less than once in long session");
    }
    const auto stats = queue.stats();
    expect(stats.submitted == count && stats.failed == expectedFailures &&
               stats.loaded == count - expectedFailures,
           "Mixed long-session outcomes were lost or double counted");
}

} // namespace

int main() {
    try {
        testPumpThreadAndCountBudget();
        testTimeBudget();
        testExceptionsAndMissingInput();
        testShutdownWithLiveJobsAndRestart();
        testSynchronousBaseline();
        testLongSessionWithMixedOutcomes();
        std::cout << "Async loading tests passed: worker decode, bounded main pump, errors, live shutdown, sync A/B\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Async loading test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
