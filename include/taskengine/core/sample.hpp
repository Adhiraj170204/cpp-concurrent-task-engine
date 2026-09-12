#ifndef TASKENGINE_CORE_SAMPLE_HPP
#define TASKENGINE_CORE_SAMPLE_HPP

#include "taskengine/core/task_id.hpp"
#include "taskengine/core/task_result.hpp"
#include "taskengine/core/task_state.hpp"

namespace taskengine {

// One measured task, in the compact form the metrics path carries.
//
// The deliberate twin of TaskResult: same identity, same outcome, same three
// instants, but without the error message. A worker records one of these per
// task into its own buffer, so the recording path never copies a string and
// never touches memory another worker is using.
//
// Trivially copyable and small, which is the whole point: merging the per-worker
// buffers after shutdown is a memcpy rather than a walk of heap-allocating
// objects.
struct Sample {
    TaskId id{};
    TaskState state{TaskState::Queued};
    TaskTimings timings{};
};

}  // namespace taskengine

#endif  // TASKENGINE_CORE_SAMPLE_HPP
