#include "taskengine/core/task.hpp"

namespace taskengine {

// The key function. A vtable and type_info are emitted alongside the first
// non-inline, non-pure virtual member of a class, which here is the destructor.
// Defining it out of line keeps them in this one object file instead of in
// every translation unit that includes the header.
Task::~Task() = default;

}  // namespace taskengine
