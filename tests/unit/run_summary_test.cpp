// Percentile arithmetic and run aggregation, checked against hand-computed
// answers. No threads and no clock here: summarize is a pure function over
// data, which is exactly what makes this testable.

#include "taskengine/metrics/run_summary.hpp"

#include "taskengine/core/sample.hpp"
#include "taskengine/core/task_result.hpp"
#include "taskengine/core/task_state.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace {

using taskengine::DurationStats;
using taskengine::nearest_rank_index;
using taskengine::RunSummary;
using taskengine::Sample;
using taskengine::summarize;
using taskengine::TaskState;
using taskengine::TaskTimings;

constexpr TaskTimings::TimePoint kOrigin{};

Sample sample_at(taskengine::TaskId id, TaskState state, int submitted_ms, int dequeued_ms,
                 int finished_ms) {
    using std::chrono::milliseconds;
    return Sample{id, state,
                  TaskTimings{kOrigin + milliseconds{submitted_ms},
                              kOrigin + milliseconds{dequeued_ms},
                              kOrigin + milliseconds{finished_ms}}};
}

// The metrics path carries these by the thousand; copying them must stay cheap.
static_assert(std::is_trivially_copyable_v<Sample>);

// --- nearest-rank percentiles ----------------------------------------------

TEST(NearestRank, MatchesHandComputedRanks) {
    // Nearest rank: the p-th percentile is the value at ceil(p * n / 100),
    // counting from one. These are the one-based ranks worked out by hand,
    // converted to zero-based indices.
    EXPECT_EQ(nearest_rank_index(50, 1), 0u);   // ceil(0.5) = 1
    EXPECT_EQ(nearest_rank_index(50, 2), 0u);   // ceil(1.0) = 1
    EXPECT_EQ(nearest_rank_index(50, 3), 1u);   // ceil(1.5) = 2
    EXPECT_EQ(nearest_rank_index(50, 4), 1u);   // ceil(2.0) = 2
    EXPECT_EQ(nearest_rank_index(50, 10), 4u);  // ceil(5.0) = 5
    EXPECT_EQ(nearest_rank_index(50, 100), 49u);

    EXPECT_EQ(nearest_rank_index(95, 1), 0u);    // ceil(0.95) = 1
    EXPECT_EQ(nearest_rank_index(95, 4), 3u);    // ceil(3.8)  = 4
    EXPECT_EQ(nearest_rank_index(95, 20), 18u);  // ceil(19.0) = 19
    EXPECT_EQ(nearest_rank_index(95, 100), 94u);
}

TEST(NearestRank, NeverIndexesPastTheEndAndHandlesAnEmptySample) {
    EXPECT_EQ(nearest_rank_index(50, 0), 0u);
    EXPECT_EQ(nearest_rank_index(100, 0), 0u);
    for (std::size_t n = 1; n <= 64; ++n) {
        EXPECT_LT(nearest_rank_index(0, n), n);
        EXPECT_LT(nearest_rank_index(50, n), n);
        EXPECT_LT(nearest_rank_index(95, n), n);
        EXPECT_LT(nearest_rank_index(100, n), n);
    }
}

TEST(NearestRank, HundredthPercentileIsTheLargestValue) {
    for (std::size_t n = 1; n <= 32; ++n) {
        EXPECT_EQ(nearest_rank_index(100, n), n - 1);
    }
}

// --- empty and near-empty samples -------------------------------------------

TEST(Summarize, AnEmptyRunProducesZerosRatherThanDividingByZero) {
    const RunSummary summary = summarize({}, 0);

    EXPECT_EQ(summary.total, 0u);
    EXPECT_EQ(summary.succeeded, 0u);
    EXPECT_EQ(summary.failed, 0u);
    EXPECT_EQ(summary.rejected, 0u);
    EXPECT_EQ(summary.queue_wait.count, 0u);
    EXPECT_EQ(summary.queue_wait.mean(), TaskTimings::Duration::zero());
    EXPECT_EQ(summary.execution_time.mean(), TaskTimings::Duration::zero());
}

TEST(Summarize, ASingleSampleIsItsOwnMinMedianAndMaximum) {
    using std::chrono::milliseconds;
    const std::vector<Sample> samples{sample_at(1, TaskState::Succeeded, 0, 10, 40)};

    const RunSummary summary = summarize(samples, 0);

    EXPECT_EQ(summary.execution_time.count, 1u);
    EXPECT_EQ(summary.execution_time.min, milliseconds{30});
    EXPECT_EQ(summary.execution_time.p50, milliseconds{30});
    EXPECT_EQ(summary.execution_time.p95, milliseconds{30});
    EXPECT_EQ(summary.execution_time.max, milliseconds{30});
    EXPECT_EQ(summary.execution_time.mean(), milliseconds{30});
}

TEST(Summarize, TwoSamplesTakeTheLowerValueAsTheMedian) {
    using std::chrono::milliseconds;
    const std::vector<Sample> samples{
        sample_at(1, TaskState::Succeeded, 0, 0, 10),
        sample_at(2, TaskState::Succeeded, 0, 0, 20),
    };

    const RunSummary summary = summarize(samples, 0);

    // Nearest rank on two values puts p50 at rank ceil(1.0) = 1: the lower one.
    EXPECT_EQ(summary.execution_time.p50, milliseconds{10});
    EXPECT_EQ(summary.execution_time.p95, milliseconds{20});
    EXPECT_EQ(summary.execution_time.mean(), milliseconds{15});
}

// --- distributions ----------------------------------------------------------

TEST(Summarize, PercentilesComeFromTheSortedSampleNotTheInputOrder) {
    using std::chrono::milliseconds;
    // Execution times 100, 10, 40, 20 ms, deliberately out of order. Sorted:
    // 10, 20, 40, 100. p50 is rank 2 (20ms), p95 is rank 4 (100ms).
    const std::vector<Sample> samples{
        sample_at(1, TaskState::Succeeded, 0, 0, 100),
        sample_at(2, TaskState::Succeeded, 0, 0, 10),
        sample_at(3, TaskState::Succeeded, 0, 0, 40),
        sample_at(4, TaskState::Succeeded, 0, 0, 20),
    };

    const RunSummary summary = summarize(samples, 0);

    EXPECT_EQ(summary.execution_time.min, milliseconds{10});
    EXPECT_EQ(summary.execution_time.p50, milliseconds{20});
    EXPECT_EQ(summary.execution_time.p95, milliseconds{100});
    EXPECT_EQ(summary.execution_time.max, milliseconds{100});
    EXPECT_EQ(summary.execution_time.total, milliseconds{170});
}

TEST(Summarize, QueueWaitAndExecutionAreMeasuredSeparately) {
    using std::chrono::milliseconds;
    const std::vector<Sample> samples{
        sample_at(1, TaskState::Succeeded, 0, 5, 15),   // wait 5,  exec 10
        sample_at(2, TaskState::Succeeded, 0, 50, 60),  // wait 50, exec 10
    };

    const RunSummary summary = summarize(samples, 0);

    // A queue that backs up shows in the wait, not in the execution time. That
    // separation is the point of recording three instants instead of two.
    EXPECT_EQ(summary.queue_wait.min, milliseconds{5});
    EXPECT_EQ(summary.queue_wait.max, milliseconds{50});
    EXPECT_EQ(summary.execution_time.min, milliseconds{10});
    EXPECT_EQ(summary.execution_time.max, milliseconds{10});
    EXPECT_EQ(summary.total_latency.min, milliseconds{15});
    EXPECT_EQ(summary.total_latency.max, milliseconds{60});
}

// --- counting ---------------------------------------------------------------

TEST(Summarize, CountsPartitionTheTotal) {
    const std::vector<Sample> samples{
        sample_at(1, TaskState::Succeeded, 0, 0, 1),
        sample_at(2, TaskState::Succeeded, 0, 0, 1),
        sample_at(3, TaskState::Failed, 0, 0, 1),
    };

    const RunSummary summary = summarize(samples, 4);

    EXPECT_EQ(summary.succeeded, 2u);
    EXPECT_EQ(summary.failed, 1u);
    EXPECT_EQ(summary.rejected, 4u);
    EXPECT_EQ(summary.total, 7u);
    // Every submitted task lands in exactly one bucket. This is the counting
    // form of the guarantee that every future is fulfilled exactly once.
    EXPECT_EQ(summary.succeeded + summary.failed + summary.rejected, summary.total);
}

TEST(Summarize, FailedTasksStillCountTowardsTheTimeSpent) {
    using std::chrono::milliseconds;
    const std::vector<Sample> samples{
        sample_at(1, TaskState::Succeeded, 0, 0, 10),
        sample_at(2, TaskState::Failed, 0, 0, 90),
    };

    const RunSummary summary = summarize(samples, 0);

    // A task that threw still occupied a worker. Excluding it would understate
    // what the run actually cost.
    EXPECT_EQ(summary.execution_time.count, 2u);
    EXPECT_EQ(summary.execution_time.max, milliseconds{90});
    EXPECT_EQ(summary.execution_time.mean(), milliseconds{50});
}

}  // namespace
