#ifndef TASKENGINE_CORE_TASK_HPP
#define TASKENGINE_CORE_TASK_HPP

namespace taskengine {

// A unit of work.
//
// Implementations override execute() and may throw from it: a throwing task is
// an expected outcome, not a defect. The engine catches at the worker boundary
// and reports the failure as a Failed TaskResult, because an exception escaping
// a thread function would call std::terminate.
//
// Ownership is always std::unique_ptr<Task>. Tasks carry no engine state: the
// id, timings and result channel live in the envelope the engine wraps around
// them, so a Task can be constructed and tested with no engine present.
class Task {
public:
    // Declared here, defined out of line in src/core/task.cpp, so that the
    // vtable and type_info are emitted in one translation unit rather than in
    // every one that includes this header.
    virtual ~Task();

    virtual void execute() = 0;

protected:
    // Protected rather than public, the usual rule for a polymorphic base. A
    // derived type can still copy or move itself, but code holding a Task&
    // cannot assign one object over another and silently slice off the derived
    // part. Declaring all five also records that this is deliberate.
    Task() = default;
    Task(const Task&) = default;
    Task& operator=(const Task&) = default;
    Task(Task&&) = default;
    Task& operator=(Task&&) = default;
};

}  // namespace taskengine

#endif  // TASKENGINE_CORE_TASK_HPP
