// Single-threaded behaviour of TaskEnvelope: what it reports, and the
// exactly-once fulfilment guarantee. No threads here; the pool that drives
// envelopes across threads is covered in tests/concurrency.

#include "taskengine/core/task_envelope.hpp"

#include "taskengine/core/task.hpp"
#include "taskengine/core/task_result.hpp"
#include "taskengine/core/task_state.hpp"

#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {

using taskengine::Task;
using taskengine::TaskEnvelope;
using taskengine::TaskResult;
using taskengine::TaskState;
using taskengine::TaskTimings;

class NoopTask final : public Task {
public:
    void execute() override {}
};

class ThrowingTask final : public Task {
public:
    void execute() override { throw std::runtime_error{"task failed"}; }
};

// Throws something not derived from std::exception, which the catch-all has to
// handle without letting it escape onto a worker thread.
class ThrowsNonExceptionTask final : public Task {
public:
    void execute() override { throw 42; }
};

std::unique_ptr<Task> noop() { return std::make_unique<NoopTask>(); }

// I2: move-only, by the rule of zero. It owns a unique_ptr and a promise.
static_assert(!std::is_copy_constructible_v<TaskEnvelope>);
static_assert(!std::is_copy_assignable_v<TaskEnvelope>);
static_assert(std::is_move_constructible_v<TaskEnvelope>);

TEST(TaskEnvelope, CarriesItsIdAndStampsSubmissionOnConstruction) {
    const TaskTimings::TimePoint before = TaskTimings::Clock::now();
    TaskEnvelope envelope{7, noop()};
    const TaskTimings::TimePoint after = TaskTimings::Clock::now();

    EXPECT_EQ(envelope.id(), 7u);
    // There is no constructor that omits the submission instant, so it cannot
    // be left unset.
    EXPECT_GE(envelope.submitted(), before);
    EXPECT_LE(envelope.submitted(), after);

    envelope.reject(TaskTimings::Clock::now());  // keep the promise satisfied
}

TEST(TaskEnvelope, RunReportsSuccessWithOrderedTimings) {
    TaskEnvelope envelope{1, noop()};
    std::future<TaskResult> future = envelope.get_future();

    const TaskTimings::TimePoint dequeued = TaskTimings::Clock::now();
    envelope.run(dequeued);

    const TaskResult result = future.get();
    EXPECT_EQ(result.id(), 1u);
    EXPECT_EQ(result.state(), TaskState::Succeeded);
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.error().empty());

    // submitted <= dequeued <= finished, so no duration can be negative.
    EXPECT_GE(result.timings().queue_wait(), TaskTimings::Duration::zero());
    EXPECT_GE(result.timings().execution_time(), TaskTimings::Duration::zero());
    EXPECT_EQ(result.timings().queue_wait() + result.timings().execution_time(),
              result.timings().total_latency());
}

TEST(TaskEnvelope, AThrownStdExceptionBecomesAFailedResultCarryingItsMessage) {
    TaskEnvelope envelope{2, std::make_unique<ThrowingTask>()};
    std::future<TaskResult> future = envelope.get_future();

    // run() is noexcept: the exception must not escape, because on a worker
    // thread that would call std::terminate.
    envelope.run(TaskTimings::Clock::now());

    const TaskResult result = future.get();
    EXPECT_EQ(result.state(), TaskState::Failed);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().message(), "task failed");
}

TEST(TaskEnvelope, AThrownNonExceptionIsReportedRatherThanSwallowed) {
    TaskEnvelope envelope{3, std::make_unique<ThrowsNonExceptionTask>()};
    std::future<TaskResult> future = envelope.get_future();

    envelope.run(TaskTimings::Clock::now());

    const TaskResult result = future.get();
    EXPECT_EQ(result.state(), TaskState::Failed);
    // The type is lost, but the failure is still visible and counted.
    EXPECT_EQ(result.error().message(), "unknown exception");
}

TEST(TaskEnvelope, RejectReportsNoExecutionWindow) {
    TaskEnvelope envelope{4, noop()};
    std::future<TaskResult> future = envelope.get_future();

    envelope.reject(TaskTimings::Clock::now());

    const TaskResult result = future.get();
    EXPECT_EQ(result.id(), 4u);
    EXPECT_EQ(result.state(), TaskState::Rejected);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error().empty());
    // A rejected task never reached a worker.
    EXPECT_EQ(result.timings().execution_time(), TaskTimings::Duration::zero());
}

TEST(TaskEnvelope, FailureReportingDoesNotDependOnTheFutureBeingRetrieved) {
    // Nothing forces a caller to keep the future. Fulfilment must still happen,
    // or an envelope that nobody is watching would behave differently from one
    // that is watched.
    TaskEnvelope envelope{5, std::make_unique<ThrowingTask>()};
    envelope.run(TaskTimings::Clock::now());
    SUCCEED();
}

TEST(TaskEnvelope, MovingAnEnvelopeCarriesTheResultChannelWithIt) {
    TaskEnvelope source{6, noop()};
    std::future<TaskResult> future = source.get_future();

    TaskEnvelope sink = std::move(source);
    sink.run(TaskTimings::Clock::now());

    // The future was taken from the original object, but the promise moved, so
    // the result still arrives. This is what makes it safe for the queue to
    // move envelopes around.
    const TaskResult result = future.get();
    EXPECT_EQ(result.id(), 6u);
    EXPECT_EQ(result.state(), TaskState::Succeeded);
}

TEST(TaskEnvelope, GetFutureIsSingleUse) {
    TaskEnvelope envelope{8, noop()};
    std::future<TaskResult> first = envelope.get_future();

    // std::promise allows exactly one future per shared state. A second
    // retrieval is a programming error and says so rather than handing back a
    // second handle that would never resolve.
    EXPECT_THROW((void)envelope.get_future(), std::future_error);

    envelope.run(TaskTimings::Clock::now());
    EXPECT_TRUE(first.get().ok());
}

}  // namespace
