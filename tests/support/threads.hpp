#ifndef TASKENGINE_TESTS_SUPPORT_THREADS_HPP
#define TASKENGINE_TESTS_SUPPORT_THREADS_HPP

// Thread coordination and thread observation for tests. Shared by the
// concurrency and stress suites; test code only, never linked into the library.
//
// Coordination (Gate, Barrier) exists so that no test synchronises threads with
// a sleep or a spin. Observation (the /proc helpers) exists so that "no thread
// is left behind" is measured rather than assumed.

#include "taskengine/concurrency/thread_pool.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>

namespace taskengine::test {

// A one-shot gate. Blocks callers of wait() until somebody calls open(), then
// lets every current and future waiter through. Opening twice is harmless,
// which is what lets several threads race to open it on the same event.
class Gate {
public:
    void open() {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            open_ = true;
        }
        condition_.notify_all();
    }

    void wait() {
        std::unique_lock<std::mutex> lock{mutex_};
        condition_.wait(lock, [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool open_{false};
};

// C++17 has no std::barrier. Blocks each caller until `count` callers have
// arrived, which is how a test proves several threads really run at once.
class Barrier {
public:
    explicit Barrier(int count) : remaining_(count) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock{mutex_};
        if (--remaining_ == 0) {
            condition_.notify_all();
            return;
        }
        condition_.wait(lock, [this] { return remaining_ == 0; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    int remaining_;
};

// Ids of every thread in this process, from /proc/self/task. Linux-specific,
// which the project already is.
//
// Entries can vanish while the directory is being read, so the error_code
// overloads are used and a vanished entry is simply not reported.
inline std::set<int> live_thread_ids() {
    std::set<int> ids;
    std::error_code error;
    std::filesystem::directory_iterator it{"/proc/self/task", error};
    const std::filesystem::directory_iterator end;
    for (; !error && it != end; it.increment(error)) {
        ids.insert(std::stoi(it->path().filename().string()));
    }
    return ids;
}

// Threads alive now that were not in `baseline`.
inline std::size_t threads_beyond(const std::set<int>& baseline) {
    std::size_t extra = 0;
    for (const int id : live_thread_ids()) {
        if (baseline.count(id) == 0) {
            ++extra;
        }
    }
    return extra;
}

// A baseline taken after any runtime-owned background threads exist.
//
// A sanitizer runtime creates a background thread lazily, on the first
// pthread_create in the process. Creating and destroying a throwaway pool first
// forces it into existence, so it is part of the baseline rather than being
// mistaken for a leaked worker. The throwaway worker may still be listed in the
// snapshot for a moment after its join, which is harmless: comparisons against
// the baseline are subset checks, not counts.
inline std::set<int> thread_baseline() {
    { const ThreadPool warmup{1, 1}; }
    return live_thread_ids();
}

// Waits until every thread not in `baseline` has disappeared from
// /proc/self/task, up to a generous limit. Returns false if any remain.
//
// This is the one bounded poll in the test suites, and there is no correct
// alternative to it. pthread_join returns when the kernel clears the thread id
// and wakes the joiner, which happens in mm_release during exit, before
// release_task removes the thread's /proc entry. So a thread that has been
// correctly joined can still be listed for a short while afterwards, and the
// kernel publishes no event to wait on for that final step. The limit exists
// only so that a genuinely leaked thread fails the test instead of hanging it;
// a correct pool returns long before it is reached.
[[nodiscard]] inline bool wait_until_no_threads_beyond(
    const std::set<int>& baseline, std::chrono::seconds limit = std::chrono::seconds{10}) {
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + limit;
    while (threads_beyond(baseline) != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

}  // namespace taskengine::test

#endif  // TASKENGINE_TESTS_SUPPORT_THREADS_HPP
