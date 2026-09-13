// Stress coverage: high volume, small tasks, and shutdown arriving in the
// middle of live traffic. This is the configuration most likely to expose a
// lost wakeup, a leaked thread or a promise left unfulfilled, and it is the
// suite that matters most under ThreadSanitizer.
//
// Nothing here synchronises with a sleep. Where a test has to wait for the
// engine to reach a state it waits on a real predicate that is certain to
// become true.
//
// These tests are deliberately larger than the concurrency suite and carry the
// "stress" label so the fast suites stay usable during development.

#include "taskengine/execution/task_engine.hpp"

#include "taskengine/concurrency/blocking_queue.hpp"
#include "taskengine/concurrency/thread_pool.hpp"
#include "taskengine/core/task.hpp"
#include "taskengine/core/task_envelope.hpp"
#include "taskengine/core/task_result.hpp"
#include "taskengine/core/task_state.hpp"
#include "taskengine/metrics/run_summary.hpp"
#include "taskengine/tasks/compute_task.hpp"

#include "support/threads.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using taskengine::BlockingQueue;
using taskengine::ComputeTask;
using taskengine::RunSummary;
using taskengine::Task;
using taskengine::TaskEngine;
using taskengine::TaskEnvelope;
using taskengine::TaskId;
using taskengine::TaskResult;
using taskengine::TaskState;
using taskengine::ThreadPool;
using taskengine::test::Gate;

// The smallest possible task: the engine overhead is all that is left, which is
// what makes a lost wakeup show up rather than hide behind real work.
class TrivialTask final : public Task {
public:
    explicit TrivialTask(std::atomic<std::uint64_t>& counter) noexcept : counter_(counter) {}
    void execute() override { counter_.fetch_add(1, std::memory_order_relaxed); }

private:
    std::atomic<std::uint64_t>& counter_;
};

class AlwaysThrowsTask final : public Task {
public:
    void execute() override { throw std::runtime_error{"stress failure"}; }
};

bool is_ready(const std::future<TaskResult>& future) {
    return future.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
}

// --- volume -----------------------------------------------------------------

TEST(EngineStress, HighVolumeOfTinyTasksLosesNothing) {
    constexpr std::size_t kTasks = 200000;
    std::atomic<std::uint64_t> executed{0};

    TaskEngine engine{8, 256};
    for (std::size_t i = 0; i < kTasks; ++i) {
        // Future discarded: the counts come from the summary, and a
        // promise-backed future does not block on destruction. Discarding it
        // avoids holding a promise shared state per task; memory still grows by
        // one 40-byte Sample per task (D33), which is why the volume is bounded.
        (void)engine.submit(std::make_unique<TrivialTask>(executed));
    }
    engine.shutdown();

    const RunSummary summary = engine.summary();
    EXPECT_EQ(executed.load(), kTasks);
    EXPECT_EQ(summary.succeeded, kTasks);
    EXPECT_EQ(summary.failed, 0u);
    EXPECT_EQ(summary.rejected, 0u);
    EXPECT_EQ(summary.total, kTasks);
    // Every sample was recorded, which means every worker's buffer was merged.
    EXPECT_EQ(summary.execution_time.count, kTasks);
}

TEST(EngineStress, ATinyQueueForcesConstantBackpressureWithoutLosingWork) {
    // Capacity one means almost every submission blocks. If a wakeup were ever
    // dropped this is where it would surface, and the run would simply stop.
    constexpr std::size_t kTasks = 20000;
    std::atomic<std::uint64_t> executed{0};

    TaskEngine engine{4, 1};
    for (std::size_t i = 0; i < kTasks; ++i) {
        (void)engine.submit(std::make_unique<TrivialTask>(executed));
    }
    engine.shutdown();

    EXPECT_EQ(executed.load(), kTasks);
    EXPECT_EQ(engine.summary().succeeded, kTasks);
}

TEST(EngineStress, ManySubmittersAndManyWorkersAccountForEveryTask) {
    constexpr std::size_t kSubmitters = 8;
    constexpr std::size_t kPerSubmitter = 20000;
    constexpr std::size_t kTotal = kSubmitters * kPerSubmitter;
    std::atomic<std::uint64_t> executed{0};

    {
        TaskEngine engine{8, 128};
        std::vector<std::thread> submitters;
        submitters.reserve(kSubmitters);
        for (std::size_t s = 0; s < kSubmitters; ++s) {
            submitters.emplace_back([&] {
                for (std::size_t i = 0; i < kPerSubmitter; ++i) {
                    (void)engine.submit(std::make_unique<TrivialTask>(executed));
                }
            });
        }
        for (auto& submitter : submitters) {
            submitter.join();
        }
        engine.shutdown();

        const RunSummary summary = engine.summary();
        EXPECT_EQ(summary.total, kTotal);
        EXPECT_EQ(summary.succeeded, kTotal);
        EXPECT_EQ(summary.succeeded + summary.failed + summary.rejected, summary.total);
        // Ids are unique, so the last one issued equals the number submitted.
        EXPECT_EQ(engine.submitted_count(), static_cast<TaskId>(kTotal));
    }
    EXPECT_EQ(executed.load(), kTotal);
}

TEST(EngineStress, MixedSuccessAndFailureIsCountedExactly) {
    constexpr std::size_t kTasks = 60000;
    constexpr std::size_t kEveryNth = 3;
    std::atomic<std::uint64_t> executed{0};

    TaskEngine engine{6, 256};
    std::size_t expected_failures = 0;
    for (std::size_t i = 0; i < kTasks; ++i) {
        if ((i + 1) % kEveryNth == 0) {
            ++expected_failures;
            (void)engine.submit(std::make_unique<AlwaysThrowsTask>());
        } else {
            (void)engine.submit(std::make_unique<TrivialTask>(executed));
        }
    }
    engine.shutdown();

    const RunSummary summary = engine.summary();
    EXPECT_EQ(summary.failed, expected_failures);
    EXPECT_EQ(summary.succeeded, kTasks - expected_failures);
    EXPECT_EQ(summary.total, kTasks);
    EXPECT_EQ(executed.load(), kTasks - expected_failures);
    // A task that threw still occupied a worker, so it is measured.
    EXPECT_EQ(summary.execution_time.count, kTasks);
}

// --- shutdown under load ----------------------------------------------------

TEST(EngineStress, AbortUnderLoadAccountsForEverySubmission) {
    // Submission races an abort, so the split between run and refused is not
    // predictable. What must hold is that the two add up and nothing vanishes.
    //
    // Two gates make the interesting parts happen by construction rather than
    // by luck. The abort waits until submission has begun, so it lands on live
    // traffic instead of an idle engine. Each submitter then makes one final
    // submission after the abort has returned, so the refusal path is certain
    // to be exercised and at least kSubmitters rejections are guaranteed.
    //
    // An earlier draft classified a submission as accepted when its future was
    // not yet ready. That was wrong: a fast worker can finish an accepted task
    // before the check, so every submission could be misread as refused and the
    // wait for an acceptance would never end.
    constexpr std::size_t kSubmitters = 6;
    constexpr std::size_t kPerSubmitter = 20000;
    constexpr std::size_t kTotal = kSubmitters * (kPerSubmitter + 1);
    std::atomic<std::uint64_t> executed{0};
    Gate submission_started;
    Gate aborted;

    TaskEngine engine{6, 64};
    std::vector<std::thread> submitters;
    submitters.reserve(kSubmitters);
    for (std::size_t s = 0; s < kSubmitters; ++s) {
        submitters.emplace_back([&] {
            for (std::size_t i = 0; i < kPerSubmitter; ++i) {
                (void)engine.submit(std::make_unique<TrivialTask>(executed));
                if (i == 0) {
                    submission_started.open();
                }
            }
            aborted.wait();
            // Certain to be refused: the engine was shut down before this runs.
            (void)engine.submit(std::make_unique<TrivialTask>(executed));
        });
    }

    submission_started.wait();
    engine.shutdown_now();
    aborted.open();

    for (auto& submitter : submitters) {
        submitter.join();
    }

    const RunSummary summary = engine.summary();
    EXPECT_EQ(summary.total, kTotal);
    EXPECT_EQ(summary.succeeded + summary.failed + summary.rejected, summary.total);
    EXPECT_EQ(summary.succeeded, executed.load());
    EXPECT_GE(summary.rejected, kSubmitters);
}

TEST(EngineStress, RepeatedConstructionAndDestructionLeavesNoThreadsBehind) {
    // The lifetime property, hammered. A cycle that failed to join would leave
    // threads alive; a cycle that destroyed the queue before its workers would
    // be reported by AddressSanitizer.
    const std::set<int> baseline = taskengine::test::thread_baseline();
    std::atomic<std::uint64_t> executed{0};

    for (int cycle = 0; cycle < 200; ++cycle) {
        TaskEngine engine{4, 16};
        for (int i = 0; i < 20; ++i) {
            (void)engine.submit(std::make_unique<TrivialTask>(executed));
        }
        // No explicit shutdown on half the cycles: the destructor has to drain.
        if (cycle % 2 == 0) {
            engine.shutdown();
        }
    }

    EXPECT_TRUE(taskengine::test::wait_until_no_threads_beyond(baseline))
        << "threads from destroyed engines are still alive";
    EXPECT_EQ(executed.load(), 200u * 20u);
}

TEST(EngineStress, EveryFutureResolvesAcrossAFullVolumeRun) {
    // I3 at volume: every future handed out resolves, to a terminal state,
    // with no broken promise. Futures are kept this time, precisely so that
    // each one can be checked.
    constexpr std::size_t kTasks = 50000;
    std::atomic<std::uint64_t> executed{0};
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kTasks);

    {
        TaskEngine engine{8, 128};
        for (std::size_t i = 0; i < kTasks; ++i) {
            futures.push_back(engine.submit(std::make_unique<TrivialTask>(executed)));
        }
        engine.shutdown_now();
    }

    std::size_t unresolved = 0;
    std::size_t broken = 0;
    std::size_t non_terminal = 0;
    for (auto& future : futures) {
        if (!is_ready(future)) {
            ++unresolved;
            continue;
        }
        try {
            if (!taskengine::is_terminal(future.get().state())) {
                ++non_terminal;
            }
        } catch (const std::future_error&) {
            ++broken;
        }
    }
    EXPECT_EQ(unresolved, 0u);
    EXPECT_EQ(broken, 0u);
    EXPECT_EQ(non_terminal, 0u);
}

// --- the queue on its own ---------------------------------------------------

TEST(QueueStress, ConservationHoldsAtVolumeWithATinyQueue) {
    // The queue without the engine around it, at a capacity that forces both
    // producers and consumers to block on nearly every operation.
    constexpr int kProducers = 6;
    constexpr int kConsumers = 6;
    constexpr int kPerProducer = 40000;
    constexpr int kTotal = kProducers * kPerProducer;

    BlockingQueue<int> queue{2};
    std::vector<std::atomic<int>> seen(kTotal);
    for (auto& counter : seen) {
        counter.store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                int value = p * kPerProducer + i;
                queue.push(std::move(value));
            }
        });
    }

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumers);
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (const std::optional<int> item = queue.pop()) {
                seen[static_cast<std::size_t>(*item)].fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }
    queue.close();
    for (auto& consumer : consumers) {
        consumer.join();
    }

    int missing = 0;
    int duplicated = 0;
    for (int i = 0; i < kTotal; ++i) {
        const int count = seen[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        if (count == 0) {
            ++missing;
        } else if (count > 1) {
            ++duplicated;
        }
    }
    EXPECT_EQ(missing, 0);
    EXPECT_EQ(duplicated, 0);
}

}  // namespace
