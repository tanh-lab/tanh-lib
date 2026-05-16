# This disables the default behavior of adding all targets to the CTest dashboard.
set_property(GLOBAL PROPERTY CTEST_TARGETS_ADDED 1)

# enable ctest
include(CTest)

# Shared googletest setup (also used by downstream consumers like cosmos).
include(${CMAKE_CURRENT_LIST_DIR}/gtest-deps.cmake)

# googlebenchmark — tanh-lib's own benchmarks only; not exposed downstream.
include(FetchContent)

FetchContent_Declare(googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
    GIT_TAG v1.9.1
    SYSTEM)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googlebenchmark)

target_compile_options(benchmark PRIVATE -w)
target_compile_options(benchmark_main PRIVATE -w)
