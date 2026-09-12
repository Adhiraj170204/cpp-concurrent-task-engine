#include "taskengine/metrics/run_summary.hpp"

#include "taskengine/core/task_state.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace taskengine {
namespace {

using Duration = TaskTimings::Duration;

// Builds the distribution from an already-collected set of durations. Takes by
// value because it sorts, and sorting the caller's data would be a surprise.
DurationStats stats_from(std::vector<Duration> values) {
    DurationStats stats;
    stats.count = values.size();
    if (values.empty()) {
        return stats;
    }

    std::sort(values.begin(), values.end());

    stats.min = values.front();
    stats.max = values.back();
    stats.p50 = values[nearest_rank_index(50, values.size())];
    stats.p95 = values[nearest_rank_index(95, values.size())];

    Duration total = Duration::zero();
    for (const Duration value : values) {
        total += value;
    }
    stats.total = total;
    return stats;
}

}  // namespace

Duration DurationStats::mean() const noexcept {
    if (count == 0) {
        return Duration::zero();
    }
    return total / static_cast<Duration::rep>(count);
}

std::size_t nearest_rank_index(unsigned percent, std::size_t n) noexcept {
    if (n == 0) {
        return 0;
    }
    // ceil(percent * n / 100) in integer arithmetic. Floating point would be
    // exact enough in practice but introduces a rounding question nobody should
    // have to think about when reading a benchmark.
    const std::size_t rank = (static_cast<std::size_t>(percent) * n + 99) / 100;
    const std::size_t clamped = std::min(std::max<std::size_t>(rank, 1), n);
    return clamped - 1;  // one-based rank to zero-based index
}

RunSummary summarize(const std::vector<Sample>& samples, std::size_t rejected) {
    RunSummary summary;
    summary.rejected = rejected;
    summary.total = samples.size() + rejected;

    std::vector<Duration> queue_wait;
    std::vector<Duration> execution;
    std::vector<Duration> latency;
    queue_wait.reserve(samples.size());
    execution.reserve(samples.size());
    latency.reserve(samples.size());

    for (const Sample& sample : samples) {
        switch (sample.state) {
            case TaskState::Succeeded:
                ++summary.succeeded;
                break;
            case TaskState::Failed:
                ++summary.failed;
                break;
            case TaskState::Rejected:
                // A rejected task never reaches a worker, so it is never
                // recorded as a sample; it arrives through the rejected count.
                // Seeing one here would mean the engine had lost track of where
                // work goes.
                ++summary.rejected;
                break;
            case TaskState::Queued:
            case TaskState::Running:
                // Not terminal. A sample is only written once a task is
                // finished, so this cannot happen without a bug upstream, and
                // silently folding it into a count would hide that bug.
                break;
        }

        queue_wait.push_back(sample.timings.queue_wait());
        execution.push_back(sample.timings.execution_time());
        latency.push_back(sample.timings.total_latency());
    }

    summary.queue_wait = stats_from(std::move(queue_wait));
    summary.execution_time = stats_from(std::move(execution));
    summary.total_latency = stats_from(std::move(latency));
    return summary;
}

}  // namespace taskengine
