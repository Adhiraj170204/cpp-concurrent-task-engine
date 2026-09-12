#ifndef TASKENGINE_METRICS_RUN_SUMMARY_HPP
#define TASKENGINE_METRICS_RUN_SUMMARY_HPP

#include "taskengine/core/sample.hpp"
#include "taskengine/core/task_result.hpp"

#include <cstddef>
#include <vector>

namespace taskengine {

// Distribution of one duration across a run.
//
// Percentiles use the nearest-rank method on the sorted sample: the p-th
// percentile is the value at index ceil(p * n / 100), counting from one. Stated
// explicitly and computed in integer arithmetic, because "p95" means different
// things in different tools and a benchmark number nobody can reproduce is
// worth nothing.
struct DurationStats {
    using Duration = TaskTimings::Duration;

    std::size_t count{};
    Duration min{};
    Duration p50{};
    Duration p95{};
    Duration max{};
    Duration total{};

    // Zero for an empty sample, rather than a division by zero.
    [[nodiscard]] Duration mean() const noexcept;
};

// What one run of the engine did.
//
// succeeded, failed and rejected partition total: every submitted task lands in
// exactly one of them, which is the counting form of the guarantee that every
// future is fulfilled exactly once.
struct RunSummary {
    std::size_t total{};
    std::size_t succeeded{};
    std::size_t failed{};
    std::size_t rejected{};

    // Over tasks that reached a worker, successful or not. A task that threw
    // still occupied a worker for a measurable time, so excluding it would
    // understate the cost of the run.
    DurationStats queue_wait{};
    DurationStats execution_time{};
    DurationStats total_latency{};
};

// Index of the p-th percentile under the nearest-rank method, counting from
// zero, for a sorted sample of n values. Exposed because it is the one piece of
// percentile arithmetic worth testing directly against hand-computed answers.
[[nodiscard]] std::size_t nearest_rank_index(unsigned percent, std::size_t n) noexcept;

// Aggregates samples recorded by workers, plus the count of tasks that were
// refused before ever reaching one.
//
// A pure function over data: no threads, no clock, no shared state. That is
// what makes the percentile arithmetic testable with hand-written inputs, and
// what keeps the measurement code incapable of perturbing what it measures.
[[nodiscard]] RunSummary summarize(const std::vector<Sample>& samples, std::size_t rejected);

}  // namespace taskengine

#endif  // TASKENGINE_METRICS_RUN_SUMMARY_HPP
