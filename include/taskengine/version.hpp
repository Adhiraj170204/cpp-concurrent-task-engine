#ifndef TASKENGINE_VERSION_HPP
#define TASKENGINE_VERSION_HPP

#include <string_view>

namespace taskengine {

// Version of the engine library.
//
// Declared here and defined in src/version.cpp rather than being an inline
// constant. That is deliberate: the executable must genuinely link the library
// archive, which is the one thing this build foundation exists to prove. An
// inline constexpr would be resolved entirely in the header and let the linker
// discard libtaskengine.a without anyone noticing.
[[nodiscard]] std::string_view version() noexcept;

}  // namespace taskengine

#endif  // TASKENGINE_VERSION_HPP
