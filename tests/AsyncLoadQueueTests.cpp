#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/resources/async_load_queue.h"
#include "engine/resources/loaders.h"
#include "engine/resources/image_mips.h"

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

class TestUploadTicket final : public engine::rhi::IUploadTicket {
public:
    mutable std::atomic_bool completed{false};
    mutable std::atomic_size_t waits{0};
    std::size_t acquires{0};
    const std::thread::id owner = std::this_thread::get_id();
    bool isComplete() const override { return completed.load(std::memory_order_acquire); }
    std::uint64_t value() const override { return 7; }
    void wait() const override { ++waits; completed.store(true, std::memory_order_release); }
    void acquire() override {
        expect(isComplete(), "Resource acquired before the GPU fence completed");
        expect(std::this_thread::get_id() == owner, "Graphics acquire escaped renderer thread");
        ++acquires;
    }
};

void testDeferredGpuFenceAndNoHeadOfLineBlocking() {
    JobSystem jobs(true, 2U);
    AsyncLoadQueue queue;
    queue.start(&jobs);
    queue.setUploadBudget(1U, 1000.0);
    auto ticket = std::make_shared<TestUploadTicket>();
    bool published = false;
    queue.submitPrepared([&]() -> AsyncLoadQueue::Prepared {
        return {[&]() { published = true; return true; }, ticket, 4096};
    }, {});
    queue.submit([]() -> AsyncLoadQueue::Finalize { return []() { return true; }; }, {});
    waitForCpu(queue);
    queue.pump();
    expect(!published && queue.stats().loaded == 1 && queue.stats().pendingGpu == 1,
           "An unfinished GPU upload was published or blocked other completions");
    expect(ticket->waits == 0 && ticket->acquires == 0, "Normal frame waited on the CPU for a GPU fence");
    ticket->completed.store(true, std::memory_order_release);
    queue.pump();
    expect(published && ticket->acquires == 1 && ticket->waits == 0,
           "GPU completion did not acquire exactly once before publication");
    const auto stats = queue.stats();
    expect(stats.pendingUploads == 0 && stats.pendingGpu == 0 && stats.gpuUploads == 1 &&
           stats.gpuBytes == 4096 && stats.lastGpuFence == 7, "GPU upload statistics are incorrect");
    queue.pump();
    expect(ticket->acquires == 1, "GPU resource was published twice");
}

void testDeferredGpuShutdownAndSynchronousWait() {
    JobSystem jobs(true, 2U);
    AsyncLoadQueue queue;
    queue.start(&jobs);
    auto ticket = std::make_shared<TestUploadTicket>();
    bool cancelled = false;
    queue.submitPrepared([ticket]() -> AsyncLoadQueue::Prepared {
        return {[]() -> bool { throw std::runtime_error("Cancelled asset must not publish"); }, ticket, 8};
    }, [&](const std::string&) {
        expect(ticket->isComplete(), "Cancellation released GPU payload before fence completion");
        cancelled = true;
    });
    waitForCpu(queue);
    queue.stop();
    expect(cancelled && ticket->waits == 1 && ticket->acquires == 0 && queue.stats().cancelled == 1,
           "Shutdown did not drain a GPU upload before cancellation");
    queue.start(&jobs);
    queue.setAsyncEnabled(false);
    auto syncTicket = std::make_shared<TestUploadTicket>();
    queue.submitPrepared([syncTicket]() -> AsyncLoadQueue::Prepared {
        return {[]() { return true; }, syncTicket, 16};
    }, {});
    expect(syncTicket->waits == 1 && syncTicket->acquires == 1 && queue.stats().loaded == 1,
           "Synchronous prepared upload did not wait, acquire and finalize");
}

void testGpuCompletionBackpressure(const std::size_t explicitLimit = 0U) {
    JobSystem jobs(true, 2U);
    AsyncLoadQueue queue;
    queue.start(&jobs);
    expect(queue.stats().inFlightLimit == 4U, "Default in-flight limit changed");
    queue.setInFlightLimit(explicitLimit);
    const auto expected = explicitLimit != 0U ? explicitLimit : 4U;
    std::atomic_size_t submitted{0};
    auto ticket = std::make_shared<TestUploadTicket>();
    for (int index = 0; index < 24; ++index) {
        queue.submitPrepared([&]() -> AsyncLoadQueue::Prepared {
            submitted.fetch_add(1U);
            return {[]() { return true; }, ticket, 16};
        }, {});
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (submitted.load() < expected || queue.stats().pendingGpu < expected || queue.stats().pendingCpu != 24U - expected) {
        expect(std::chrono::steady_clock::now() < deadline, "GPU backpressure test jobs did not start");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int frame = 0; frame < 10; ++frame) { queue.pump(); }
    expect(submitted.load() == expected && queue.stats().pendingGpu == expected &&
           queue.stats().pendingCpu == 24U - expected && queue.stats().inFlightLimit == expected,
           "GPU-pending completions escaped the staging/backpressure limit");
    queue.stop();
    expect(queue.stats().cancelled == 24 && ticket->acquires == 0,
           "GPU-pending backpressure backlog was not cancelled safely");
    bool rejected = false;
    try { queue.setInFlightLimit(17U); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "Unsafe asset in-flight limit was accepted");
}

void testMipChain() {
    using engine::resources::buildRgba8MipTail;
    const std::vector<std::uint8_t> pixel{10, 20, 30, 40};
    expect(buildRgba8MipTail(1, 1, pixel).empty(), "1x1 texture must have no mip tail");
    std::vector<std::uint8_t> source(7 * 5 * 4, 127);
    const auto tail = buildRgba8MipTail(7, 5, source);
    expect(tail.size() == 2 && tail[0].width == 3 && tail[0].height == 2 &&
           tail[1].width == 1 && tail[1].height == 1, "NPOT mip chain has incorrect dimensions");
    for (const auto& level : tail) {
        for (const auto channel : level.pixels) { expect(channel == 127, "Mip generation changed a constant color"); }
    }
    const std::vector<std::uint8_t> odd{0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255};
    expect(buildRgba8MipTail(3, 1, odd)[0].pixels[0] == 85, "Odd texture edge was discarded");
    bool rejected = false;
    try { (void)buildRgba8MipTail(2, 2, pixel); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "Invalid mip source was accepted");
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
        testDeferredGpuFenceAndNoHeadOfLineBlocking();
        testDeferredGpuShutdownAndSynchronousWait();
        testGpuCompletionBackpressure();
        testGpuCompletionBackpressure(3U);
        testGpuCompletionBackpressure(16U);
        testMipChain();
        std::cout << "Async loading tests passed: worker decode, bounded pump, errors, shutdown, GPU fence readiness/acquire, CPU mips\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Async loading test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
