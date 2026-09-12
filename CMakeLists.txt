# 3.22 specifically: it is the CMake in Ubuntu 22.04 LTS, and the version that
# introduced policy CMP0128. Below that, CXX_EXTENSIONS OFF is silently ignored
# whenever the compiler's default standard already satisfies the requested one
# -- GCC 13 defaults to gnu++17, so the build would compile as gnu++17 while
# claiming to be strict C++17.
cmake_minimum_required(VERSION 3.22)

project(TaskEngine
        VERSION 0.1.0
        DESCRIPTION "Concurrent task processing engine"
        LANGUAGES CXX)

find_package(Threads REQUIRED)

# C++17 with no compiler extensions: the compiler is invoked as -std=c++17
# rather than -std=gnu++17, so non-standard constructs fail to compile instead
# of being silently accepted. Verified by checking that __STRICT_ANSI__ is
# defined in the build, not merely by setting the variable.
set(CMAKE_CXX_EXTENSIONS OFF)

# Single-configuration generators (Make, Ninja) have no build type unless one
# is given. Debug is the development default; Release is selected explicitly,
# and is the only configuration benchmarks may use (ARCHITECTURE.md 8.5).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
               Debug Release RelWithDebInfo MinSizeRel)
endif()

# Consumed by clangd and other editor tooling. Generated, never committed.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release optimisation level, pinned rather than inherited. CMake's default for
# this build type is -O3; -O2 is the benchmark baseline (ARCHITECTURE.md 8.5),
# so the measured configuration is stated in the build rather than depending on
# whatever CMake's default happens to be. -DNDEBUG is retained to keep asserts
# out of Release. Debug is untouched.
#
# Unlike the warning policy below, this is deliberately a build-type variable
# rather than a per-target option: an optimisation level is correct to apply to
# every object in a Release build, third-party code included. -Werror is not.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# Warning policy (ARCHITECTURE.md 9.2). Defined once and attached PRIVATE to
# our own targets only -- never through CMAKE_CXX_FLAGS, which would also
# apply it to third-party code fetched at configure time, where an upstream
# change could break our build for reasons unrelated to our code.
# GCC/Clang syntax; D14 commits the project to Linux/GCC.
set(TASKENGINE_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Werror)

# ---------------------------------------------------------------------------
# Sanitizers (ARCHITECTURE.md 9.1). ASan and TSan are mutually exclusive
# runtimes that cannot share a binary, so this is one choice rather than a set
# of toggles, and each selection wants its own build directory.
#
# Applied at directory scope, which reaches GoogleTest as well. That is
# deliberate, and is the one place the "our targets only" rule of
# ARCHITECTURE.md 9.2 does not apply: a sanitizer only reasons about
# instrumented code, so mixing instrumented and uninstrumented objects produces
# both missed races and false reports. Section 9.3 builds GoogleTest from source
# precisely so that it can be instrumented. The warning policy stays
# target-scoped, because that rule is about -Werror and not about sanitizers.
# See D22.
# ---------------------------------------------------------------------------
set(TASKENGINE_SANITIZER "off" CACHE STRING
    "Sanitizer build: off | address+undefined | thread")
set_property(CACHE TASKENGINE_SANITIZER PROPERTY STRINGS
             off address+undefined thread)

if(TASKENGINE_SANITIZER STREQUAL "address+undefined")
  # -fno-sanitize-recover makes UBSan abort instead of printing and carrying on,
  # so a violation fails the run rather than scrolling past in a log.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                      -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
elseif(TASKENGINE_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
elseif(NOT TASKENGINE_SANITIZER STREQUAL "off")
  message(FATAL_ERROR
          "TASKENGINE_SANITIZER must be off, address+undefined, or thread")
endif()

# ---------------------------------------------------------------------------
# Library: taskengine
# One library target for the whole core (D7). Static: simplest ownership and
# deployment story, and no rpath or ABI concerns in the M12 container.
# ---------------------------------------------------------------------------
add_library(taskengine STATIC
    src/concurrency/thread_pool.cpp
    src/core/task.cpp
    src/core/task_envelope.cpp
    src/version.cpp)

target_include_directories(taskengine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# PUBLIC: C++17 is part of the library's interface, so anything linking it
# inherits the requirement rather than having to restate it.
target_compile_features(taskengine PUBLIC cxx_std_17)

target_compile_options(taskengine PRIVATE ${TASKENGINE_WARNINGS})

# BlockingQueue uses std::mutex and std::condition_variable, so the threading
# library is part of what this library requires of its consumers, not an
# implementation detail. PUBLIC, so anything linking taskengine gets it.
target_link_libraries(taskengine PUBLIC Threads::Threads)

# Single source of truth for the version string: it lives in project() above
# and reaches the code from there. No fallback definition, so a build without
# it fails loudly instead of reporting a wrong version.
target_compile_definitions(taskengine PRIVATE
    TASKENGINE_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# Executable: task-engine
# ---------------------------------------------------------------------------
add_executable(task-engine
    src/main.cpp)

target_link_libraries(task-engine PRIVATE taskengine)

target_compile_options(task-engine PRIVATE ${TASKENGINE_WARNINGS})

# ---------------------------------------------------------------------------
# Tests
# The option exists because FetchContent resolves GoogleTest at *configure*
# time and therefore needs the network. Without a switch the project could not
# be configured offline, or in a container that has no reason to build tests.
# ---------------------------------------------------------------------------
option(TASKENGINE_BUILD_TESTS "Build the test suite" ON)

if(TASKENGINE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
