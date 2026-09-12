#ifndef TASKENGINE_EXECUTION_TASK_ENGINE_HPP
#define TASKENGINE_EXECUTION_TASK_ENGINE_HPP

#include "taskengine/concurrency/thread_pool.hpp"
#include "taskengine/core/task.hpp"
#include "taskengine/core/task_id.hpp"
#include "taskengine/core/task_result.hpp"
#include "taskengine/metrics/run_summary.hpp"

#include <atomic>
#include <cstddef>
#include <future>
#include <memory>

namespace taskengine {

// The engine as callers see it: hand over a task, get back a future.
//
// It owns the pool and adds the two things the pool has no business knowing
// about. Identity: every submitted task gets a monotonically increasing id,
// assigned here, never reused. And the notion of a run: the pool records
// samples per worker and counts refusals, but it has no idea what a summary is.
//
// Everything else is delegation, deliberately. The pool is a reusable
// concurrency primitive that can be tested against hand-built envelopes; this
// is the application-facing surface that the CLI, and later the HTTP and broker
// layers, talk to. Keeping them apart is what lets the pool be exercised
// without any notion of task identity, and lets this be exercised without
// reaching into envelopes.
//
// Shutdown semantics are the pool semantics, unchanged: shutdown() drains,
// shutdown_now() abandons what is queued, neither interrupts a running task,
// both are idempotent, and the destructor drains.
class TaskEngine {
public:
    // Throws std::invalid_argument if either argument is zero.
    TaskEngine(std::size_t worker_count, std::size_t queue_capacity);

    TaskEngine(const TaskEngine&) = delete;
    TaskEngine& operator=(const TaskEngine&) = delete;
    TaskEngine(TaskEngine&&) = delete;
    TaskEngine& operator=(TaskEngine&&) = delete;

    // Submits a task and returns the channel its outcome arrives on.
    //
    // The future always resolves, and always to a terminal state. A task
    // submitted after shutdown comes back Rejected rather than throwing or
    // silently disappearing, so a caller never has to ask whether the engine
    // was still open when it called.
    [[nodiscard]] std::future<TaskResult> submit(std::unique_ptr<Task> task);

    void shutdown();
    void shutdown_now();

    // What the run did. Valid only after a shutdown, and throws
    // std::logic_error otherwise rather than reporting a half-finished run as
    // though it were complete.
    [[nodiscard]] RunSummary summary() const;

    [[nodiscard]] std::size_t worker_count() const noexcept { return pool_.worker_count(); }
    [[nodiscard]] bool is_shut_down() const { return pool_.is_shut_down(); }

    // Number of ids handed out so far, which is also the number of tasks
    // submitted. Ids start at 1, so this is the last id issued.
    [[nodiscard]] TaskId submitted_count() const noexcept;

private:
    // The only atomic on the submission path. relaxed is sufficient and
    // justified: ids need to be unique, not ordered against any other memory,
    // and the queue mutex already establishes every happens-before edge the
    // envelope itself needs.
    std::atomic<TaskId> next_id_{1};

    ThreadPool pool_;
};

}  // namespace taskengine

#endif  // TASKENGINE_EXECUTION_TASK_ENGINE_HPP
