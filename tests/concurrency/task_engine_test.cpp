// The engine as callers see it: submit a task, get a future, shut down, read
// what the run did. Identity, state transitions and timing all come together
// here for the first time.

#include "taskengine/execution/task_engine.hpp"

#include "taskengine/core/task.hpp"
#include "taskengine/core/task_result.hpp"
#include "taskengine/core/task_state.hpp"
#include "taskengine/metrics/run_summary.hpp"
#include "taskengine/tasks/compute_task.hpp"
#include "taskengine/tasks/sleep_task.hpp"

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

#include <gtest/gtest.h>

namespace {

using taskengine::ComputeTask;
using taskengine::RunSummary;
using taskengine::SleepTask;
using taskengine::Task;
using taskengine::TaskEngine;
using taskengine::TaskId;
using taskengine::TaskResult;
using taskengine::TaskState;
using taskengine::TaskTimings;

class FailingTask final : public Task {
public:
    void execute() override { throw std::runtime_error{"deliberate failure"}; }
};

std::unique_ptr<Task> compute(std::uint64_t iterations = 200) {
    return std::make_unique<ComputeTask>(iterations, 1);
}

// --- construction -----------------------------------------------------------

TEST(TaskEngine, RejectsDegenerateConfigurations) {
    EXPECT_THROW((TaskEngine{0, 8}), std::invalid_argument);
    EXPECT_THROW((TaskEngine{2, 0}), std::invalid_argument);
}

TEST(TaskEngine, ReportsItsWorkerCount) {
    const TaskEngine engine{3, 8};
    EXPECT_EQ(engine.worker_count(), 3u);
    EXPECT_FALSE(engine.is_shut_down());
}

// --- identity ---------------------------------------------------------------

TEST(TaskEngine, AssignsEveryTaskADistinctIncreasingId) {
    constexpr int kTasks = 500;
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kTasks);

    TaskEngine engine{4, 64};
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(engine.submit(compute()));
    }
    engine.shutdown();

    std::set<TaskId> ids;
    for (auto& future : futures) {
        ids.insert(future.get().id());
    }
    // Unique and never reused. Ids start at 1, so the set spans 1..kTasks.
    EXPECT_EQ(ids.size(), static_cast<std::size_t>(kTasks));
    EXPECT_EQ(*ids.begin(), 1u);
    EXPECT_EQ(*ids.rbegin(), static_cast<TaskId>(kTasks));
    EXPECT_EQ(engine.submitted_count(), static_cast<TaskId>(kTasks));
}

TEST(TaskEngine, IdsStayUniqueWhenManyThreadsSubmitAtOnce) {
    constexpr int kSubmitters = 6;
    constexpr int kPerSubmitter = 500;
    constexpr int kTotal = kSubmitters * kPerSubmitter;

    std::vector<std::vector<std::future<TaskResult>>> per_thread(kSubmitters);

    {
        TaskEngine engine{4, 64};
        std::vector<std::thread> submitters;
        submitters.reserve(kSubmitters);
        for (int s = 0; s < kSubmitters; ++s) {
            submitters.emplace_back([&, s] {
                per_thread[static_cast<std::size_t>(s)].reserve(kPerSubmitter);
                for (int i = 0; i < kPerSubmitter; ++i) {
                    per_thread[static_cast<std::size_t>(s)].push_back(engine.submit(compute(10)));
                }
            });
        }
        for (auto& submitter : submitters) {
            submitter.join();
        }
        engine.shutdown();
    }

    std::set<TaskId> ids;
    for (auto& bucket : per_thread) {
        for (auto& future : bucket) {
            ids.insert(future.get().id());
        }
    }
    // A duplicate id would mean the counter was not atomic.
    EXPECT_EQ(ids.size(), static_cast<std::size_t>(kTotal));
}

// --- outcomes ---------------------------------------------------------------

TEST(TaskEngine, ReportsSuccessAndFailureSeparately) {
    constexpr int kGood = 60;
    constexpr int kBad = 20;
    std::vector<std::future<TaskResult>> good;
    std::vector<std::future<TaskResult>> bad;
    good.reserve(kGood);
    bad.reserve(kBad);

    TaskEngine engine{4, 128};
    for (int i = 0; i < kGood; ++i) {
        good.push_back(engine.submit(compute()));
    }
    for (int i = 0; i < kBad; ++i) {
        bad.push_back(engine.submit(std::make_unique<FailingTask>()));
    }
    engine.shutdown();

    for (auto& future : good) {
        const TaskResult result = future.get();
        EXPECT_EQ(result.state(), TaskState::Succeeded);
        EXPECT_TRUE(result.error().empty());
    }
    for (auto& future : bad) {
        const TaskResult result = future.get();
        EXPECT_EQ(result.state(), TaskState::Failed);
        EXPECT_EQ(result.error().message(), "deliberate failure");
    }

    const RunSummary summary = engine.summary();
    EXPECT_EQ(summary.succeeded, static_cast<std::size_t>(kGood));
    EXPECT_EQ(summary.failed, static_cast<std::size_t>(kBad));
    EXPECT_EQ(summary.rejected, 0u);
    EXPECT_EQ(summary.total, static_cast<std::size_t>(kGood + kBad));
}

TEST(TaskEngine, SubmittingAfterShutdownYieldsARejectedResultRatherThanAnError) {
    TaskEngine engine{2, 8};
    engine.shutdown();

    std::future<TaskResult> future = engine.submit(compute());

    // The caller never has to ask whether the engine was still open. The answer
    // arrives in the same shape as every other outcome.
    const TaskResult result = future.get();
    EXPECT_EQ(result.state(), TaskState::Rejected);
    EXPECT_TRUE(result.error().empty());

    const RunSummary summary = engine.summary();
    EXPECT_EQ(summary.rejected, 1u);
    EXPECT_EQ(summary.succeeded, 0u);
    EXPECT_EQ(summary.total, 1u);
}

// --- timing -----------------------------------------------------------------

TEST(TaskEngine, RecordsQueueWaitAndExecutionTimeSeparately) {
    constexpr int kTasks = 100;
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kTasks);

    TaskEngine engine{2, 256};
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(engine.submit(compute(2000)));
    }
    engine.shutdown();

    for (auto& future : futures) {
        const TaskResult result = future.get();
        const TaskTimings& timings = result.timings();

        // The three instants are ordered, so no duration can come out negative.
        EXPECT_GE(timings.queue_wait(), TaskTimings::Duration::zero());
        EXPECT_GE(timings.execution_time(), TaskTimings::Duration::zero());
        // The parts sum to the whole, by construction rather than by luck.
        EXPECT_EQ(timings.queue_wait() + timings.execution_time(), timings.total_latency());
    }

    const RunSummary summary = engine.summary();
    EXPECT_EQ(summary.execution_time.count, static_cast<std::size_t>(kTasks));
    EXPECT_EQ(summary.queue_wait.count, static_cast<std::size_t>(kTasks));
    EXPECT_GT(summary.execution_time.total, TaskTimings::Duration::zero());
    EXPECT_LE(summary.execution_time.min, summary.execution_time.p50);
    EXPECT_LE(summary.execution_time.p50, summary.execution_time.p95);
    EXPECT_LE(summary.execution_time.p95, summary.execution_time.max);
}

TEST(TaskEngine, ABlockingTaskShowsUpAsExecutionTimeNotQueueWait) {
    using std::chrono::microseconds;
    // One worker, one slow task: the time has to land in execution, since
    // nothing was waiting ahead of it.
    TaskEngine engine{1, 8};
    std::future<TaskResult> future = engine.submit(std::make_unique<SleepTask>(microseconds{5000}));
    engine.shutdown();

    const TaskResult result = future.get();
    EXPECT_EQ(result.state(), TaskState::Succeeded);
    EXPECT_GE(result.timings().execution_time(), microseconds{5000});
}

// --- summary discipline -----------------------------------------------------

TEST(TaskEngine, SummaryBeforeShutdownIsRefusedRatherThanGuessed) {
    TaskEngine engine{2, 8};
    (void)engine.submit(compute());

    // Reading the per-worker buffers while workers are still appending would be
    // a data race, and a partial answer that looked complete would be worse
    // than no answer at all.
    EXPECT_THROW((void)engine.summary(), std::logic_error);

    engine.shutdown();
    EXPECT_NO_THROW((void)engine.summary());
}

TEST(TaskEngine, AbortShutdownCountsAbandonedWorkAsRejected) {
    constexpr int kTasks = 2000;
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kTasks);

    TaskEngine engine{2, 16};
    std::thread submitter{[&] {
        for (int i = 0; i < kTasks; ++i) {
            futures.push_back(engine.submit(compute(50)));
        }
    }};
    submitter.join();
    engine.shutdown_now();

    const RunSummary summary = engine.summary();

    // Whatever the split turned out to be, nothing is lost and nothing is
    // counted twice.
    EXPECT_EQ(summary.succeeded + summary.failed + summary.rejected, summary.total);
    EXPECT_EQ(summary.total, static_cast<std::size_t>(kTasks));

    std::size_t terminal = 0;
    for (auto& future : futures) {
        if (taskengine::is_terminal(future.get().state())) {
            ++terminal;
        }
    }
    EXPECT_EQ(terminal, static_cast<std::size_t>(kTasks));
}

TEST(TaskEngine, EveryTaskIsAccountedForAcrossAFullRun) {
    constexpr int kTasks = 1000;
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kTasks);

    TaskEngine engine{6, 64};
    for (int i = 0; i < kTasks; ++i) {
        if (i % 10 == 0) {
            futures.push_back(engine.submit(std::make_unique<FailingTask>()));
        } else {
            futures.push_back(engine.submit(compute(20)));
        }
    }
    engine.shutdown();

    const RunSummary summary = engine.summary();
    EXPECT_EQ(summary.total, static_cast<std::size_t>(kTasks));
    EXPECT_EQ(summary.failed, static_cast<std::size_t>(kTasks / 10));
    EXPECT_EQ(summary.succeeded, static_cast<std::size_t>(kTasks - kTasks / 10));
    EXPECT_EQ(summary.rejected, 0u);
    EXPECT_EQ(summary.succeeded + summary.failed + summary.rejected, summary.total);

    // Every future resolved, and the counts above describe exactly those
    // outcomes rather than a separate tally that might drift from them.
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    for (auto& future : futures) {
        const TaskState state = future.get().state();
        if (state == TaskState::Succeeded) {
            ++succeeded;
        } else if (state == TaskState::Failed) {
            ++failed;
        }
    }
    EXPECT_EQ(succeeded, summary.succeeded);
    EXPECT_EQ(failed, summary.failed);
}

TEST(TaskEngine, DestructorDrainsWithoutAnExplicitShutdown) {
    constexpr int kTasks = 300;
    std::atomic<int> unused{0};
    (void)unused;
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kTasks);
    {
        TaskEngine engine{4, 64};
        for (int i = 0; i < kTasks; ++i) {
            futures.push_back(engine.submit(compute(20)));
        }
    }
    for (auto& future : futures) {
        EXPECT_EQ(future.get().state(), TaskState::Succeeded);
    }
}

}  // namespace
