#include "taskengine/version.hpp"

#include <string_view>

namespace taskengine {

std::string_view version() noexcept {
    // TASKENGINE_VERSION is a string literal supplied by CMake from
    // project(VERSION ...). String literals have static storage duration, so
    // the returned view refers to memory that outlives every caller and
    // cannot dangle.
    return TASKENGINE_VERSION;
}

}  // namespace taskengine
