#ifndef TASKENGINE_CONCURRENCY_THREAD_POOL_HPP
#define TASKENGINE_CONCURRENCY_THREAD_POOL_HPP

#include "taskengine/concurrency/blocking_queue.hpp"
#include "taskengine/core/task_envelope.hpp"

#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace taskengine {

// A fixed set of worker threads draining a bounded queue of task envelopes.
//
// Threads are created once, in the constructor, and never again: there is no
// thread per task. Each worker loops on the queue until the queue is closed and
// drained, then returns, and the pool joins it.
//
// Two shutdown modes, both idempotent and callable from any thread:
//
//   shutdown()      drain. Stop accepting, let everything already queued run to
//                   completion, then join.
//   shutdown_now()  abort. Stop accepting, discard what is queued and reject
//                   each discarded envelope, then join.
//
// Neither interrupts a task that is already running. There is no portable, safe
// way to do that, and pretending otherwise would be dishonest. A task that is
// in flight when shutdown is called always finishes and always reports its real
// outcome.
//
// The invariant that matters: every future handed out by submit() is fulfilled
// exactly once, on every path. Submission refused, queued then drained, running
// when shutdown arrives, throwing, or the pool being destroyed underneath it.
// A caller blocked in future::get() cannot be left waiting.
//
// Not supported: calling submit(), shutdown() or shutdown_now() from inside a
// task running on this pool. submit() can deadlock against a full queue that
// only this worker could drain, and shutdown would have the worker join itself.
class ThreadPool {
public:
    // Throws std::invalid_argument if worker_count or queue_capacity is zero.
    // Zero workers could never run anything and zero capacity could never
    // accept anything, so both are errors rather than silent hangs.
    ThreadPool(std::size_t worker_count, std::size_t queue_capacity);

    // Drain shutdown, then join. A pool never outlives its threads.
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Hands the envelope to a worker, blocking while the queue is full.
    //
    // Returns false if the pool is shut down, in which case the envelope is
    // rejected here rather than dropped, so its future still resolves.
    bool submit(TaskEnvelope envelope);

    void shutdown();
    void shutdown_now();

    [[nodiscard]] std::size_t worker_count() const noexcept { return threads_.size(); }
    [[nodiscard]] bool is_shut_down() const { return queue_.is_closed(); }

    // A momentary snapshot, for diagnostics only.
    [[nodiscard]] std::size_t queued() const { return queue_.size(); }

private:
    void worker_loop();
    void join_workers();

    // Declaration order is load-bearing, not stylistic. Members are destroyed
    // in reverse order of declaration, so threads_ is destroyed first and
    // queue_ last. The queue the workers reach into therefore outlives the
    // workers themselves. Reordering these two members would introduce a
    // use-after-free that only shows up under shutdown.
    BlockingQueue<TaskEnvelope> queue_;

    // Guards joined_ only. Taken after the queue has been closed, never before,
    // and never while queue_ internals are held: close first, join second.
    mutable std::mutex shutdown_mutex_;
    bool joined_{false};

    std::vector<std::thread> threads_;
};

}  // namespace taskengine

#endif  // TASKENGINE_CONCURRENCY_THREAD_POOL_HPP
