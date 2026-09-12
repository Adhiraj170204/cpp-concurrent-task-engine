#ifndef TASKENGINE_CORE_TASK_ENVELOPE_HPP
#define TASKENGINE_CORE_TASK_ENVELOPE_HPP

#include "taskengine/core/sample.hpp"
#include "taskengine/core/task.hpp"
#include "taskengine/core/task_id.hpp"
#include "taskengine/core/task_result.hpp"

#include <future>
#include <memory>

namespace taskengine {

// Everything the engine needs to know about one submitted task, wrapped around
// the task itself.
//
// A Task carries only the work. The identity, the submission instant and the
// channel the result travels back on live here instead, so a Task
// implementation stays free of engine plumbing and can be built in a test with
// no engine present.
//
// Move-only by the rule of zero: it owns a unique_ptr and a promise, both of
// which are move-only, so the compiler suppresses copying without being told.
//
// The guarantee this type exists to carry: the promise inside is fulfilled
// exactly once, by run() or by reject(), on every path the engine can take.
// Whoever takes ownership of an envelope takes that obligation with it.
class TaskEnvelope {
public:
    using TimePoint = TaskTimings::TimePoint;

    // Stamps the submission instant. There is no constructor that omits it, so
    // it cannot be forgotten.
    TaskEnvelope(TaskId id, std::unique_ptr<Task> task);

    // Hands back the future for this task. Callable once, as std::promise
    // requires; a second call throws future_already_retrieved.
    [[nodiscard]] std::future<TaskResult> get_future();

    // Runs the task, fulfils the promise with the outcome, and returns the
    // compact record of what happened for the metrics path.
    //
    // Returning a Sample rather than a TaskResult keeps the recording path free
    // of the error string: the message goes to the caller through the promise,
    // the measurement stays trivially copyable.
    //
    // noexcept because this executes directly on a worker thread, where an
    // escaping exception would call std::terminate. A throwing task is an
    // expected outcome and becomes a Failed result rather than a crash.
    //
    // The remaining way this can terminate is a genuine engine bug, such as an
    // envelope being fulfilled twice. Terminating is the right answer to that:
    // it means the engine has lost track of an envelope, and somebody else is
    // about to wait forever.
    //
    // Precondition: the envelope has not been moved from and has not already
    // been run or rejected.
    Sample run(TimePoint dequeued) noexcept;

    // Fulfils the promise as Rejected: this task was never handed to a worker,
    // so it has no execution window and produces no sample.
    void reject(TimePoint finished) noexcept;

    [[nodiscard]] TaskId id() const noexcept { return id_; }
    [[nodiscard]] TimePoint submitted() const noexcept { return submitted_; }

private:
    TaskId id_;
    std::unique_ptr<Task> task_;
    TimePoint submitted_;
    std::promise<TaskResult> promise_;
};

}  // namespace taskengine

#endif  // TASKENGINE_CORE_TASK_ENVELOPE_HPP
