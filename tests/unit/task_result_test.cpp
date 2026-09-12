#include "taskengine/core/task_result.hpp"

#include "taskengine/core/task_state.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using taskengine::TaskError;
using taskengine::TaskResult;
using taskengine::TaskState;
using taskengine::TaskTimings;
using taskengine::is_terminal;
using taskengine::to_string;

// A fixed origin, so the tests are arithmetic on known values rather than
// measurements of real elapsed time. Nothing here reads the clock.
constexpr TaskTimings::TimePoint kOrigin{};

TaskTimings timings_at(int submitted_ms, int dequeued_ms, int finished_ms) {
    using std::chrono::milliseconds;
    return TaskTimings{kOrigin + milliseconds{submitted_ms},
                       kOrigin + milliseconds{dequeued_ms},
                       kOrigin + milliseconds{finished_ms}};
}

// --- compile-time properties ------------------------------------------------

// TaskResult travels through std::promise, so it has to be movable.
static_assert(std::is_move_constructible_v<TaskResult>);
static_assert(std::is_move_assignable_v<TaskResult>);

// It must not be default-constructible: a result always describes a task that
// actually reached a terminal state.
static_assert(!std::is_default_constructible_v<TaskResult>);

// TaskTimings is a plain aggregate of three time points.
static_assert(std::is_trivially_copyable_v<TaskTimings>);
static_assert(std::is_aggregate_v<TaskTimings>);

// TaskError follows the rule of zero: all four copy and move operations exist.
static_assert(std::is_copy_constructible_v<TaskError>);
static_assert(std::is_move_constructible_v<TaskError>);
static_assert(std::is_copy_assignable_v<TaskError>);
static_assert(std::is_move_assignable_v<TaskError>);

// The converting constructor is explicit, so a bare string cannot silently
// become a TaskError.
static_assert(!std::is_convertible_v<std::string, TaskError>);
static_assert(std::is_constructible_v<TaskError, std::string>);

// to_string is usable in a constant expression.
static_assert(to_string(TaskState::Succeeded) == std::string_view{"Succeeded"});
static_assert(is_terminal(TaskState::Failed));
static_assert(!is_terminal(TaskState::Queued));

// --- TaskState --------------------------------------------------------------

TEST(TaskState, TerminalStatesAreExactlyTheThreeOutcomes) {
    EXPECT_FALSE(is_terminal(TaskState::Queued));
    EXPECT_FALSE(is_terminal(TaskState::Running));
    EXPECT_TRUE(is_terminal(TaskState::Succeeded));
    EXPECT_TRUE(is_terminal(TaskState::Failed));
    EXPECT_TRUE(is_terminal(TaskState::Rejected));
}

TEST(TaskState, EveryStateHasADistinctNonEmptyName) {
    // A state added without updating to_string fails the build under -Wswitch.
    // This catches the subtler mistake of two states sharing a name.
    const std::string_view names[] = {
        to_string(TaskState::Queued),
        to_string(TaskState::Running),
        to_string(TaskState::Succeeded),
        to_string(TaskState::Failed),
        to_string(TaskState::Rejected),
    };
    for (std::size_t i = 0; i < std::size(names); ++i) {
        EXPECT_FALSE(names[i].empty());
        EXPECT_NE(names[i], "Invalid");
        for (std::size_t j = i + 1; j < std::size(names); ++j) {
            EXPECT_NE(names[i], names[j]);
        }
    }
}

// --- TaskError --------------------------------------------------------------

TEST(TaskError, DefaultConstructedIsEmpty) {
    const TaskError error;
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(error.message().empty());
}

TEST(TaskError, CarriesItsMessage) {
    const TaskError error{"queue closed"};
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(error.message(), "queue closed");
}

TEST(TaskError, ConstructionFromAnRvalueMovesRatherThanCopies) {
    // Long enough to be heap-allocated rather than held in the small-string
    // buffer, so the address is stable evidence of a move.
    std::string message(1000, 'x');
    const char* const buffer = message.data();

    const TaskError error{std::move(message)};

    // Same heap buffer the caller allocated. This fails if the constructor is
    // ever changed to take a const reference and copy.
    EXPECT_EQ(error.message().data(), buffer);
    EXPECT_EQ(error.message().size(), 1000u);
}

// --- TaskResult -------------------------------------------------------------

TEST(TaskResult, SucceededCarriesNoError) {
    const TaskResult result = TaskResult::succeeded(7, timings_at(1, 2, 5));

    EXPECT_EQ(result.id(), 7u);
    EXPECT_EQ(result.state(), TaskState::Succeeded);
    EXPECT_TRUE(result.ok());
    EXPECT_TRUE(result.error().empty());
    EXPECT_TRUE(is_terminal(result.state()));
}

TEST(TaskResult, FailedCarriesTheErrorAndIsNotOk) {
    const TaskResult result =
        TaskResult::failed(9, TaskError{"task threw"}, timings_at(1, 2, 5));

    EXPECT_EQ(result.id(), 9u);
    EXPECT_EQ(result.state(), TaskState::Failed);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error().message(), "task threw");
    EXPECT_TRUE(is_terminal(result.state()));
}

TEST(TaskResult, RejectedHasNoExecutionWindow) {
    using std::chrono::milliseconds;
    const TaskTimings::TimePoint submitted = kOrigin + milliseconds{10};
    const TaskTimings::TimePoint finished = kOrigin + milliseconds{30};

    const TaskResult result = TaskResult::rejected(3, submitted, finished);

    EXPECT_EQ(result.state(), TaskState::Rejected);
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(result.error().empty());
    // A rejected task never reached a worker, so it has no execution window.
    EXPECT_EQ(result.timings().execution_time(), TaskTimings::Duration::zero());
    EXPECT_EQ(result.timings().queue_wait(), milliseconds{20});
    EXPECT_EQ(result.timings().total_latency(), milliseconds{20});
}

TEST(TaskResult, DurationsAreDerivedFromTheThreeInstants) {
    using std::chrono::milliseconds;
    const TaskResult result = TaskResult::succeeded(1, timings_at(10, 40, 100));

    EXPECT_EQ(result.timings().queue_wait(), milliseconds{30});
    EXPECT_EQ(result.timings().execution_time(), milliseconds{60});
    EXPECT_EQ(result.timings().total_latency(), milliseconds{90});
    // The parts sum to the whole.
    EXPECT_EQ(result.timings().queue_wait() + result.timings().execution_time(),
              result.timings().total_latency());
}

TEST(TaskResult, MovingTransfersTheErrorBufferAndKeepsTheRest) {
    TaskResult result =
        TaskResult::failed(4, TaskError{std::string(1000, 'y')}, timings_at(0, 1, 2));
    const char* const buffer = result.error().message().data();

    const TaskResult moved = std::move(result);

    EXPECT_EQ(moved.error().message().data(), buffer);
    EXPECT_EQ(moved.id(), 4u);
    EXPECT_EQ(moved.state(), TaskState::Failed);
}

TEST(TaskResult, CopyingProducesAnIndependentError) {
    const TaskResult original =
        TaskResult::failed(5, TaskError{std::string(1000, 'z')}, timings_at(0, 1, 2));

    const TaskResult copy = original;  // NOLINT: copy is the point of the test

    EXPECT_EQ(copy.error().message(), original.error().message());
    // A deep copy: separate buffers holding equal contents.
    EXPECT_NE(copy.error().message().data(), original.error().message().data());
}

TEST(TaskResult, IdIsCarriedThroughEveryFactory) {
    using std::chrono::milliseconds;
    EXPECT_EQ(TaskResult::succeeded(11, timings_at(0, 1, 2)).id(), 11u);
    EXPECT_EQ(TaskResult::failed(12, TaskError{"e"}, timings_at(0, 1, 2)).id(), 12u);
    EXPECT_EQ(TaskResult::rejected(13, kOrigin, kOrigin + milliseconds{1}).id(), 13u);
}

}  // namespace
