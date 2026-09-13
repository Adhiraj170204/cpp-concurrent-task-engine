// Threaded behaviour of ThreadPool: worker lifecycle, both shutdown modes,
// exception containment, and the invariant that every future handed out is
// fulfilled exactly once.
//
// Nothing here synchronises with a sleep. Where a test has to wait for the pool
// to reach a state, it waits on a real predicate rather than on the clock.
//
// Several tests would hang rather than fail if the pool stopped releasing its
// workers. That is deliberate: a timeout naming one test is a clearer signal
// than a silently wrong count.

#include "taskengine/concurrency/thread_pool.hpp"

#include "taskengine/core/task.hpp"
#include "taskengine/core/task_envelope.hpp"
#include "taskengine/core/task_result.hpp"
#include "taskengine/core/task_state.hpp"

#include "support/threads.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

using taskengine::Task;
using taskengine::TaskEnvelope;
using taskengine::TaskId;
using taskengine::TaskResult;
using taskengine::TaskState;
using taskengine::ThreadPool;

using taskengine::test::Barrier;
using taskengine::test::Gate;

class CountingTask final : public Task {
public:
    explicit CountingTask(std::atomic<int>& executions) noexcept : executions_(executions) {}
    void execute() override { executions_.fetch_add(1, std::memory_order_relaxed); }

private:
    std::atomic<int>& executions_;
};

class GatedTask final : public Task {
public:
    GatedTask(Gate& started, Gate& release) noexcept : started_(started), release_(release) {}
    void execute() override {
        started_.open();
        release_.wait();
    }

private:
    Gate& started_;
    Gate& release_;
};

class BarrierTask final : public Task {
public:
    explicit BarrierTask(Barrier& barrier) noexcept : barrier_(barrier) {}
    void execute() override { barrier_.arrive_and_wait(); }

private:
    Barrier& barrier_;
};

class ThrowingTask final : public Task {
public:
    void execute() override { throw std::runtime_error{"boom"}; }
};

std::unique_ptr<Task> counting(std::atomic<int>& executions) {
    return std::make_unique<CountingTask>(executions);
}

std::future<TaskResult> submit(ThreadPool& pool, TaskId id, std::unique_ptr<Task> task) {
    TaskEnvelope envelope{id, std::move(task)};
    std::future<TaskResult> future = envelope.get_future();
    pool.submit(std::move(envelope));
    return future;
}

// Waits for the pool to publish its closed state.
//
// This is not the sleep-based synchronisation the testing strategy forbids.
// is_shut_down() becomes true at the exact instant the queue is closed and
// drained, so this waits on a real predicate that is certain to become true.
// No duration is being guessed at.
//
// A condition variable is not available here without adding a test-only hook
// to the production pool, because the transition being waited for happens
// inside shutdown_now itself. This is the one yield on a predicate that remains
// in the suites, and it stays for that reason.
void wait_until_shut_down(const ThreadPool& pool) {
    while (!pool.is_shut_down()) {
        std::this_thread::yield();
    }
}

bool is_ready(const std::future<TaskResult>& future) {
    // Zero timeout: a readiness query, not a wait.
    return future.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
}

// --- construction -----------------------------------------------------------

TEST(ThreadPool, RejectsZeroWorkers) {
    // A pool with no workers could never run anything, so it is an error rather
    // than a queue that silently fills up forever.
    EXPECT_THROW((ThreadPool{0, 8}), std::invalid_argument);
}

TEST(ThreadPool, RejectsZeroQueueCapacity) {
    EXPECT_THROW((ThreadPool{2, 0}), std::invalid_argument);
}

TEST(ThreadPool, StartsExactlyTheRequestedNumberOfWorkers) {
    const std::set<int> baseline = taskengine::test::thread_baseline();
    {
        const ThreadPool pool{4, 8};
        EXPECT_EQ(pool.worker_count(), 4u);
        EXPECT_EQ(taskengine::test::threads_beyond(baseline), 4u);
        EXPECT_FALSE(pool.is_shut_down());
    }
    // I4 and the M4 acceptance criterion: no threads left behind. Had any
    // worker still been joinable, destroying the thread vector would have
    // called std::terminate and this process would already be gone.
    EXPECT_TRUE(taskengine::test::wait_until_no_threads_beyond(baseline))
        << "worker threads outlived their pool";
}

// --- execution --------------------------------------------------------------

TEST(ThreadPool, ASingleWorkerRunsEverySubmittedTask) {
    constexpr int kTasks = 200;
    std::atomic<int> executions{0};
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kTasks);
    {
        ThreadPool pool{1, 16};
        for (int i = 0; i < kTasks; ++i) {
            futures.push_back(submit(pool, static_cast<TaskId>(i), counting(executions)));
        }
        pool.shutdown();
    }
    EXPECT_EQ(executions.load(), kTasks);
    for (auto& future : futures) {
        EXPECT_EQ(future.get().state(), TaskState::Succeeded);
    }
}

TEST(ThreadPool, MultipleWorkersRunEverySubmittedTask) {
    constexpr int kTasks = 2000;
    std::atomic<int> executions{0};
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kTasks);
    {
        ThreadPool pool{8, 32};
        for (int i = 0; i < kTasks; ++i) {
            futures.push_back(submit(pool, static_cast<TaskId>(i), counting(executions)));
        }
        pool.shutdown();
    }
    EXPECT_EQ(executions.load(), kTasks);
    for (auto& future : futures) {
        EXPECT_EQ(future.get().state(), TaskState::Succeeded);
    }
}

TEST(ThreadPool, WorkersRunConcurrentlyRatherThanOneAtATime) {
    // Four tasks that each wait for all four to arrive. With fewer than four
    // running at once this cannot complete, so a pool that quietly serialised
    // its work would hang here rather than pass.
    constexpr int kWorkers = 4;
    Barrier barrier{kWorkers};
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kWorkers);
    {
        ThreadPool pool{kWorkers, kWorkers};
        for (int i = 0; i < kWorkers; ++i) {
            TaskEnvelope envelope{static_cast<TaskId>(i), std::make_unique<BarrierTask>(barrier)};
            futures.push_back(envelope.get_future());
            ASSERT_TRUE(pool.submit(std::move(envelope)));
        }
        pool.shutdown();
    }
    for (auto& future : futures) {
        EXPECT_EQ(future.get().state(), TaskState::Succeeded);
    }
}

// --- exception containment --------------------------------------------------

TEST(ThreadPool, AThrowingTaskBecomesAFailedResultAndTheWorkerSurvives) {
    constexpr int kAfter = 50;
    std::atomic<int> executions{0};
    std::future<TaskResult> failed;
    std::vector<std::future<TaskResult>> after;
    after.reserve(kAfter);
    {
        ThreadPool pool{1, 16};
        failed = submit(pool, 1, std::make_unique<ThrowingTask>());
        // The same worker has to keep serving afterwards. Had the exception
        // escaped the worker loop, this process would be gone.
        for (int i = 0; i < kAfter; ++i) {
            after.push_back(submit(pool, static_cast<TaskId>(100 + i), counting(executions)));
        }
        pool.shutdown();
    }
    const TaskResult result = failed.get();
    EXPECT_EQ(result.state(), TaskState::Failed);
    EXPECT_EQ(result.error().message(), "boom");
    EXPECT_EQ(executions.load(), kAfter);
    for (auto& future : after) {
        EXPECT_EQ(future.get().state(), TaskState::Succeeded);
    }
}

// --- shutdown ---------------------------------------------------------------

TEST(ThreadPool, DrainShutdownRunsEverythingAlreadyQueued) {
    constexpr int kTasks = 500;
    std::atomic<int> executions{0};
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kTasks);

    ThreadPool pool{2, 1024};
    for (int i = 0; i < kTasks; ++i) {
        futures.push_back(submit(pool, static_cast<TaskId>(i), counting(executions)));
    }
    pool.shutdown();

    // Drain means drain: nothing that was accepted is abandoned.
    EXPECT_EQ(executions.load(), kTasks);
    for (auto& future : futures) {
        ASSERT_TRUE(is_ready(future));
        EXPECT_EQ(future.get().state(), TaskState::Succeeded);
    }
}

TEST(ThreadPool, AbortShutdownRejectsQueuedWorkButLetsTheRunningTaskFinish) {
    constexpr int kQueued = 5;
    Gate started;
    Gate release;
    std::atomic<int> executions{0};

    ThreadPool pool{1, 64};

    // Occupy the only worker. Until released it cannot take anything else.
    TaskEnvelope running{1, std::make_unique<GatedTask>(started, release)};
    std::future<TaskResult> running_future = running.get_future();
    ASSERT_TRUE(pool.submit(std::move(running)));
    started.wait();

    std::vector<std::future<TaskResult>> queued;
    queued.reserve(kQueued);
    for (int i = 0; i < kQueued; ++i) {
        queued.push_back(submit(pool, static_cast<TaskId>(100 + i), counting(executions)));
    }
    ASSERT_EQ(pool.queued(), static_cast<std::size_t>(kQueued));

    // Release the running task only once the abort has closed and drained the
    // queue, so the worker cannot pick up queued work in between. This is what
    // makes the test deterministic instead of a race.
    std::thread releaser{[&] {
        wait_until_shut_down(pool);
        release.open();
    }};

    pool.shutdown_now();
    releaser.join();

    // Work already in flight completes and reports its real outcome. Shutdown
    // never interrupts a running task.
    EXPECT_EQ(running_future.get().state(), TaskState::Succeeded);

    // Everything still queued was abandoned, and every one of those callers was
    // told so rather than left waiting.
    EXPECT_EQ(executions.load(), 0);
    for (auto& future : queued) {
        ASSERT_TRUE(is_ready(future));
        EXPECT_EQ(future.get().state(), TaskState::Rejected);
    }
}

TEST(ThreadPool, ShutdownIsIdempotentAndBothModesCanBeMixed) {
    std::atomic<int> executions{0};
    ThreadPool pool{2, 16};
    std::future<TaskResult> future = submit(pool, 1, counting(executions));

    pool.shutdown();
    pool.shutdown();
    pool.shutdown_now();
    pool.shutdown();

    EXPECT_TRUE(pool.is_shut_down());
    EXPECT_EQ(future.get().state(), TaskState::Succeeded);
}

TEST(ThreadPool, SubmitAfterShutdownIsRefusedAndTheCallerIsToldWhy) {
    ThreadPool pool{2, 16};
    pool.shutdown();

    TaskEnvelope envelope{99, std::make_unique<ThrowingTask>()};
    std::future<TaskResult> future = envelope.get_future();

    EXPECT_FALSE(pool.submit(std::move(envelope)));

    // The refusal still resolves the future. Dropping the envelope would leave
    // the caller holding a broken promise instead of an answer.
    ASSERT_TRUE(is_ready(future));
    const TaskResult result = future.get();
    EXPECT_EQ(result.state(), TaskState::Rejected);
    EXPECT_EQ(result.id(), 99u);
}

TEST(ThreadPool, DestructorDrainsAndJoins) {
    constexpr int kTasks = 300;
    const std::set<int> baseline = taskengine::test::thread_baseline();
    std::atomic<int> executions{0};
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kTasks);
    {
        ThreadPool pool{4, 64};
        for (int i = 0; i < kTasks; ++i) {
            futures.push_back(submit(pool, static_cast<TaskId>(i), counting(executions)));
        }
        // No explicit shutdown: the destructor has to do it.
    }
    EXPECT_TRUE(taskengine::test::wait_until_no_threads_beyond(baseline))
        << "worker threads outlived their pool";
    EXPECT_EQ(executions.load(), kTasks);
    for (auto& future : futures) {
        ASSERT_TRUE(is_ready(future));
        EXPECT_EQ(future.get().state(), TaskState::Succeeded);
    }
}

// --- the central invariant --------------------------------------------------

TEST(ThreadPool, EveryFutureIsFulfilledExactlyOnceUnderLoadAndAbortShutdown) {
    // I3. Submission races an abort shutdown, so which tasks run and which are
    // refused is not predictable. Every future must still resolve, to a
    // terminal state, without a broken promise. One left unfulfilled would
    // block its caller forever.
    constexpr int kSubmitters = 4;
    constexpr int kPerSubmitter = 1000;
    constexpr int kTotal = kSubmitters * kPerSubmitter;

    std::atomic<int> executions{0};
    std::atomic<int> accepted{0};
    std::vector<std::future<TaskResult>> futures(kTotal);
    Gate first_accepted;

    {
        ThreadPool pool{4, 16};

        std::vector<std::thread> submitters;
        submitters.reserve(kSubmitters);
        for (int s = 0; s < kSubmitters; ++s) {
            submitters.emplace_back([&, s] {
                for (int i = 0; i < kPerSubmitter; ++i) {
                    const int index = s * kPerSubmitter + i;
                    TaskEnvelope envelope{static_cast<TaskId>(index), counting(executions)};
                    futures[static_cast<std::size_t>(index)] = envelope.get_future();
                    if (pool.submit(std::move(envelope))) {
                        // Exactly one thread sees the counter at zero, so the
                        // gate opens once without a lock on every submission.
                        if (accepted.fetch_add(1, std::memory_order_relaxed) == 0) {
                            first_accepted.open();
                        }
                    }
                }
            });
        }

        // Wait until a submission has actually been accepted before aborting.
        //
        // Without this, the abort can close the queue before any submitter has
        // reached its first push, every submission is refused, and the accepted
        // path is never exercised -- which showed up as a flake under
        // AddressSanitizer, where thread start-up is slower. A gate opened by
        // the first accepted submission makes it certain, with no polling.
        first_accepted.wait();

        pool.shutdown_now();

        for (auto& submitter : submitters) {
            submitter.join();
        }
        // Submitters carried on after the abort. Those submissions were
        // refused, and each refusal fulfilled its own future.
        pool.shutdown_now();
    }

    int unfulfilled = 0;
    int broken = 0;
    int non_terminal = 0;
    for (auto& future : futures) {
        if (!is_ready(future)) {
            ++unfulfilled;
            continue;
        }
        try {
            const TaskResult result = future.get();
            if (!taskengine::is_terminal(result.state())) {
                ++non_terminal;
            }
        } catch (const std::future_error&) {
            // A broken promise means an envelope was destroyed unfulfilled,
            // which is exactly what this invariant forbids.
            ++broken;
        }
    }

    EXPECT_EQ(unfulfilled, 0);
    EXPECT_EQ(broken, 0);
    EXPECT_EQ(non_terminal, 0);
    EXPECT_GT(accepted.load(), 0);
}

TEST(ThreadPool, EveryFutureIsFulfilledWhenThePoolIsDestroyedUnderneathThem) {
    constexpr int kTasks = 400;
    std::atomic<int> executions{0};
    std::vector<std::future<TaskResult>> futures;
    futures.reserve(kTasks);
    {
        ThreadPool pool{3, 32};
        for (int i = 0; i < kTasks; ++i) {
            futures.push_back(submit(pool, static_cast<TaskId>(i), counting(executions)));
        }
    }
    // The futures outlive the pool, and every one of them still has an answer.
    for (auto& future : futures) {
        ASSERT_TRUE(is_ready(future));
        EXPECT_NO_THROW({
            const TaskResult result = future.get();
            EXPECT_TRUE(taskengine::is_terminal(result.state()));
        });
    }
}

}  // namespace
