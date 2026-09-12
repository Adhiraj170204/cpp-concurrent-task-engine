// The two concrete task types. ComputeTask has to be deterministic or it is
// useless as a benchmark workload; SleepTask has to actually wait.

#include "taskengine/tasks/compute_task.hpp"
#include "taskengine/tasks/sleep_task.hpp"

#include "taskengine/core/task.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <type_traits>

namespace {

using taskengine::ComputeTask;
using taskengine::SleepTask;
using taskengine::Task;

// Both are usable through the interface, which is what the engine holds.
static_assert(std::is_base_of_v<Task, ComputeTask>);
static_assert(std::is_base_of_v<Task, SleepTask>);
static_assert(std::is_final_v<ComputeTask>);
static_assert(std::is_final_v<SleepTask>);

TEST(ComputeTask, ProducesTheSameChecksumForTheSameInput) {
    ComputeTask first{5000, 12345};
    ComputeTask second{5000, 12345};

    first.execute();
    second.execute();

    // Determinism is the whole requirement. A workload whose result varies
    // between runs would make every benchmark number unreproducible.
    EXPECT_EQ(first.checksum(), second.checksum());
    EXPECT_NE(first.checksum(), 0u);
}

TEST(ComputeTask, DifferentSeedsProduceDifferentChecksums) {
    ComputeTask a{5000, 1};
    ComputeTask b{5000, 2};

    a.execute();
    b.execute();

    EXPECT_NE(a.checksum(), b.checksum());
}

TEST(ComputeTask, MoreIterationsChangeTheResult) {
    ComputeTask few{100, 7};
    ComputeTask many{200, 7};

    few.execute();
    many.execute();

    // If these matched, the loop would not be doing per-iteration work and the
    // iteration count would not control the size of the workload.
    EXPECT_NE(few.checksum(), many.checksum());
}

TEST(ComputeTask, ZeroIterationsDoesNothingAndIsNotAnError) {
    ComputeTask task{0};
    task.execute();
    // Used by the benchmark to measure the engine overhead floor: the cost of
    // moving a task through the system with no work in it.
    EXPECT_EQ(task.checksum(), 0u);
    EXPECT_EQ(task.iterations(), 0u);
}

TEST(ComputeTask, RunsThroughTheBaseInterface) {
    const std::unique_ptr<Task> task = std::make_unique<ComputeTask>(1000, 3);
    task->execute();
    SUCCEED();
}

TEST(SleepTask, WaitsAtLeastTheRequestedDuration) {
    using std::chrono::microseconds;
    using std::chrono::steady_clock;

    constexpr microseconds kRequested{2000};
    SleepTask task{kRequested};

    const steady_clock::time_point start = steady_clock::now();
    task.execute();
    const steady_clock::duration elapsed = steady_clock::now() - start;

    // A lower bound only. sleep_for guarantees at least the requested duration
    // and may overshoot by any amount, so an equality or an upper bound here
    // would be a test that fails on a loaded machine.
    EXPECT_GE(elapsed, kRequested);
    EXPECT_EQ(task.duration(), kRequested);
}

TEST(SleepTask, ZeroDurationReturnsPromptly) {
    SleepTask task{std::chrono::microseconds{0}};
    task.execute();
    SUCCEED();
}

}  // namespace
