# bench-task-engine: the repeatable worker-scaling benchmark (ARCHITECTURE.md 8).
# A separate binary, so benchmark-only code never links into the product path.
add_executable(bench-task-engine
    bench_task_engine.cpp)

target_link_libraries(bench-task-engine PRIVATE taskengine)

target_compile_options(bench-task-engine PRIVATE ${TASKENGINE_WARNINGS})

# The harness records how it was built, and refuses to measure a sanitizer build
# or a non-Release build, so it needs to know both. The flags recorded are the
# ones for the build type actually configured, not always the Release ones.
string(TOUPPER "${CMAKE_BUILD_TYPE}" TASKENGINE_BUILD_TYPE_UPPER)
target_compile_definitions(bench-task-engine PRIVATE
    TASKENGINE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    TASKENGINE_SANITIZER_MODE="${TASKENGINE_SANITIZER}"
    TASKENGINE_OPTIMISATION_FLAGS="${CMAKE_CXX_FLAGS_${TASKENGINE_BUILD_TYPE_UPPER}}")

if(TASKENGINE_BUILD_TESTS)
  # Proves the harness runs end to end without taking a minute. Allowed in any
  # build, sanitizer builds included, because it is explicitly not a measurement.
  add_test(NAME BenchHarness.SmokeRunCompletes COMMAND bench-task-engine --smoke)
  set_tests_properties(BenchHarness.SmokeRunCompletes PROPERTIES
      LABELS integration
      TIMEOUT 600)
endif()
