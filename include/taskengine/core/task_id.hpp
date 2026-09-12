#ifndef TASKENGINE_CORE_TASK_ID_HPP
#define TASKENGINE_CORE_TASK_ID_HPP

#include <cstdint>

namespace taskengine {

// Identity of a submitted task: monotonically increasing, assigned at
// submission, never reused.
//
// A plain alias rather than a strong type. It is an opaque counter used only
// for identity and diagnostics, and no other integer appears in these
// interfaces that it could be confused with. A strong type would have to carry
// its own comparison, hashing and streaming support to earn its keep.
using TaskId = std::uint64_t;

}  // namespace taskengine

#endif  // TASKENGINE_CORE_TASK_ID_HPP
