#include "taskengine/version.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// version() is the only behaviour that exists at this milestone. These
// assertions test properties the implementation actually promises, rather than
// asserting something that would hold no matter what the code did.

TEST(Version, IsNotEmpty) {
    EXPECT_FALSE(taskengine::version().empty());
}

TEST(Version, RefersToStaticStorageAndCannotDangle) {
    // version() returns a view of a string literal, which has static storage
    // duration, so repeated calls must yield the same pointer. If the
    // implementation were ever changed to return a view of a temporary -- a
    // std::string built on the fly, say -- the view would dangle and this
    // assertion is what would catch it.
    const std::string_view first = taskengine::version();
    const std::string_view second = taskengine::version();

    EXPECT_EQ(first.data(), second.data());
    EXPECT_EQ(first, second);
}

}  // namespace
