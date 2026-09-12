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
# Library: taskengine
# One library target for the whole core (D7). Static: simplest ownership and
# deployment story, and no rpath or ABI concerns in the M12 container.
# ---------------------------------------------------------------------------
add_library(taskengine STATIC
    src/version.cpp)

target_include_directories(taskengine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# PUBLIC: C++17 is part of the library's interface, so anything linking it
# inherits the requirement rather than having to restate it.
target_compile_features(taskengine PUBLIC cxx_std_17)

target_compile_options(taskengine PRIVATE ${TASKENGINE_WARNINGS})

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
