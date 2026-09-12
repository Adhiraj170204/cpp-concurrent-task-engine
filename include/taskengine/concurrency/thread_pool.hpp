#ifndef TASKENGINE_CONCURRENCY_THREAD_POOL_HPP
#define TASKENGINE_CONCURRENCY_THREAD_POOL_HPP

#include "taskengine/concurrency/blocking_queue.hpp"
#include "taskengine/core/sample.hpp"
#include "taskengine/core/task_envelope.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
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
// way to do that, and pretending otherwise would be dishonest. A task in flight
// when shutdown arrives always finishes and always reports its real outcome.
//
// The invariant that matters: every future handed out through an envelope this
// pool accepted is fulfilled exactly once, on every path. Submission refused,
// queued then discarded, running when shutdown arrives, throwing, or the pool
// destroyed underneath it. A caller blocked in future::get() is never left
// waiting.
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

    // Every sample recorded by every worker, merged into one sequence.
    //
    // Valid only after the workers have been joined, and throws std::logic_error
    // otherwise. Reading the buffers while workers are still writing to them
    // would be a data race, and returning a partial answer that looked complete
    // would be worse than refusing.
    [[nodiscard]] std::vector<Sample> collect_samples() const;

    // Tasks refused before reaching a worker: submitted after shutdown, or
    // discarded by an abort. They never ran, so they have no sample.
    [[nodiscard]] std::uint64_t rejected_count() const noexcept;

    [[nodiscard]] std::size_t worker_count() const noexcept { return threads_.size(); }
    [[nodiscard]] bool is_shut_down() const { return queue_.is_closed(); }

    // A momentary snapshot, for diagnostics only.
    [[nodiscard]] std::size_t queued() const { return queue_.size(); }

private:
    void worker_loop(std::size_t worker_index);
    void join_workers();
    [[nodiscard]] bool workers_joined() const;

    // Declaration order is load-bearing, not stylistic. Members are destroyed
    // in reverse order of declaration, so threads_ is destroyed first and
    // queue_ and samples_ last. Both of those are touched by workers for their
    // whole lifetime, so they have to outlive the workers. Reordering these
    // members would compile, read fine, and introduce a use-after-free that
    // only appears during destruction.
    BlockingQueue<TaskEnvelope> queue_;

    // One buffer per worker, sized once before any thread starts and never
    // resized. Worker i appends only to samples_[i], so the recording path
    // needs no synchronisation at all and cannot contend with another worker.
    // A shared counter here would be a contention point that distorted exactly
    // the worker-scaling curve the benchmarks exist to measure.
    std::vector<std::vector<Sample>> samples_;

    // Rejections happen on the caller thread, not a worker, so they cannot go
    // into a per-worker buffer. An atomic is acceptable here precisely because
    // this is not the hot path: a rejection is a terminal, rare event.
    std::atomic<std::uint64_t> rejected_{0};

    // Guards joined_ only. Taken after the queue has been closed, never before,
    // and never while queue_ internals are held: close first, join second.
    mutable std::mutex shutdown_mutex_;
    bool joined_{false};

    std::vector<std::thread> threads_;
};

}  // namespace taskengine

#endif  // TASKENGINE_CONCURRENCY_THREAD_POOL_HPP
