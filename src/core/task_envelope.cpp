#include "taskengine/core/task_envelope.hpp"

#include "taskengine/core/task_state.hpp"

#include <exception>
#include <utility>

namespace taskengine {

TaskEnvelope::TaskEnvelope(TaskId id, std::unique_ptr<Task> task)
    : id_(id), task_(std::move(task)), submitted_(TaskTimings::Clock::now()) {}

std::future<TaskResult> TaskEnvelope::get_future() { return promise_.get_future(); }

Sample TaskEnvelope::run(TimePoint dequeued) noexcept {
    TaskError error;
    bool failed = false;

    try {
        task_->execute();
    } catch (const std::exception& e) {
        error = TaskError{e.what()};
        failed = true;
    } catch (...) {
        // Something not derived from std::exception was thrown. The type is
        // lost, but the failure is still reported rather than swallowed.
        error = TaskError{"unknown exception"};
        failed = true;
    }

    const TimePoint finished = TaskTimings::Clock::now();
    const TaskTimings timings{submitted_, dequeued, finished};
    const TaskState state = failed ? TaskState::Failed : TaskState::Succeeded;

    if (failed) {
        promise_.set_value(TaskResult::failed(id_, std::move(error), timings));
    } else {
        promise_.set_value(TaskResult::succeeded(id_, timings));
    }

    return Sample{id_, state, timings};
}

void TaskEnvelope::reject(TimePoint finished) noexcept {
    promise_.set_value(TaskResult::rejected(id_, submitted_, finished));
}

}  // namespace taskengine
