# GoogleTest is declared here rather than at the top level so that the product
# library and the CLI never see it: nothing outside this directory can acquire
# a dependency on the test framework by accident.
#
# Built from source via FetchContent rather than find_package (D15,
# ARCHITECTURE.md 9.3): a prebuilt system libgtest is not instrumented, so
# ThreadSanitizer would not observe races inside the test framework itself.
include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  # GoogleTest v1.17.0, pinned to its immutable commit rather than to the tag
  # name: a tag can be repointed upstream, a commit cannot.
  GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
)

# gmock is not used. Skipping it removes roughly half of the dependency's
# build time and keeps the footprint to what we actually depend on (I7).
set(BUILD_GMOCK OFF)

# Do not let GoogleTest contribute install rules to this project.
set(INSTALL_GTEST OFF)

FetchContent_MakeAvailable(googletest)

# ---------------------------------------------------------------------------
# Unit tests -- CTest label "unit" (ARCHITECTURE.md 10.1)
# One test binary per label group; "concurrency" and "stress" arrive with the
# code they test (ARCHITECTURE.md 0.1).
# ---------------------------------------------------------------------------
add_executable(taskengine_unit_tests
    unit/blocking_queue_test.cpp
    unit/options_test.cpp
    unit/run_summary_test.cpp
    unit/task_envelope_test.cpp
    unit/task_result_test.cpp
    unit/tasks_test.cpp
    unit/task_test.cpp
    unit/version_test.cpp)

# taskengine carries cxx_std_17 as a PUBLIC compile feature, so the test target
# inherits the standard rather than restating it.
target_link_libraries(taskengine_unit_tests PRIVATE
    taskengine
    GTest::gtest_main)

# Our warning policy, applied to our test code only. The GoogleTest targets
# were created by FetchContent above and never receive these flags, so an
# upstream change cannot fail our build through -Werror.
target_compile_options(taskengine_unit_tests PRIVATE ${TASKENGINE_WARNINGS})

# Registers each TEST() as its own CTest entry rather than one opaque entry for
# the whole binary, so a failure names the case that failed.
include(GoogleTest)
gtest_discover_tests(taskengine_unit_tests
    PROPERTIES LABELS unit)

# ---------------------------------------------------------------------------
# Concurrency tests -- CTest label "concurrency" (ARCHITECTURE.md 10.1)
# A separate binary so the fast unit suite can be run on its own during
# development, and so the threaded suite can be pointed at a TSan build.
# ---------------------------------------------------------------------------
add_executable(taskengine_concurrency_tests
    concurrency/blocking_queue_concurrency_test.cpp
    concurrency/task_engine_test.cpp
    concurrency/thread_pool_test.cpp)

target_link_libraries(taskengine_concurrency_tests PRIVATE
    taskengine
    GTest::gtest_main)

# Shared test-support headers live under tests/support.
target_include_directories(taskengine_concurrency_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(taskengine_concurrency_tests PRIVATE ${TASKENGINE_WARNINGS})

gtest_discover_tests(taskengine_concurrency_tests
    PROPERTIES LABELS concurrency)

# ---------------------------------------------------------------------------
# Stress tests -- CTest label "stress" (ARCHITECTURE.md 10.4)
# High volume and shutdown under live load. Larger than the concurrency suite,
# so kept separate to leave the fast suites usable during development, and meant
# above all to be run under ThreadSanitizer.
# ---------------------------------------------------------------------------
add_executable(taskengine_stress_tests
    stress/engine_stress_test.cpp)

target_link_libraries(taskengine_stress_tests PRIVATE
    taskengine
    GTest::gtest_main)

# Shared test-support headers live under tests/support.
target_include_directories(taskengine_stress_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(taskengine_stress_tests PRIVATE ${TASKENGINE_WARNINGS})

gtest_discover_tests(taskengine_stress_tests
    PROPERTIES LABELS stress)

# ---------------------------------------------------------------------------
# Integration tests -- CTest label "integration" (PLAN.md M7)
# Run the real task-engine executable in a child process: exit codes, stdout
# versus stderr, and graceful handling of SIGINT and SIGTERM. The path of the
# built binary is compiled in, and the dependency makes sure it exists first.
# ---------------------------------------------------------------------------
add_executable(taskengine_integration_tests
    integration/cli_integration_test.cpp)

target_link_libraries(taskengine_integration_tests PRIVATE
    GTest::gtest_main)

target_compile_features(taskengine_integration_tests PRIVATE cxx_std_17)

target_compile_options(taskengine_integration_tests PRIVATE ${TASKENGINE_WARNINGS})

target_compile_definitions(taskengine_integration_tests PRIVATE
    TASKENGINE_CLI_PATH="$<TARGET_FILE:task-engine>")

add_dependencies(taskengine_integration_tests task-engine)

gtest_discover_tests(taskengine_integration_tests
    PROPERTIES LABELS integration)
