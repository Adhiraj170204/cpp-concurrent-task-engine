#include "taskengine/execution/task_engine.hpp"

#include "taskengine/core/task_envelope.hpp"

#include <utility>

namespace taskengine {

TaskEngine::TaskEngine(std::size_t worker_count, std::size_t queue_capacity)
    : pool_(worker_count, queue_capacity) {}

std::future<TaskResult> TaskEngine::submit(std::unique_ptr<Task> task) {
    TaskEnvelope envelope{next_id_.fetch_add(1, std::memory_order_relaxed), std::move(task)};
    std::future<TaskResult> future = envelope.get_future();

    // The return value is deliberately not checked. A refusal is not an error
    // the caller has to handle separately: the pool has already fulfilled the
    // envelope as Rejected, so the future below carries that answer in the same
    // shape as every other outcome.
    (void)pool_.submit(std::move(envelope));

    return future;
}

void TaskEngine::shutdown() { pool_.shutdown(); }

void TaskEngine::shutdown_now() { pool_.shutdown_now(); }

RunSummary TaskEngine::summary() const {
    // collect_samples throws if the workers are still running, which is the
    // behaviour wanted here too: a summary of a run that has not finished would
    // be a number that looks authoritative and is not.
    return summarize(pool_.collect_samples(), pool_.rejected_count());
}

TaskId TaskEngine::submitted_count() const noexcept {
    return next_id_.load(std::memory_order_relaxed) - 1;
}

}  // namespace taskengine
