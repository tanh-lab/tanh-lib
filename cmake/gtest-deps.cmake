# Reusable googletest setup — shared between tanh-lib's own test suites and
# any downstream project that consumes tanh-lib (e.g. cosmos).
#
# Idempotent: safe to include from multiple places. If `gtest_main` already
# exists as a target, this is a no-op.
#
# After including this file:
#   - `gtest`, `gtest_main` targets are available
#   - `include(GoogleTest)` has been called → `gtest_discover_tests` works
#   - `tanh_copy_dlls_for_tests(<target>)` function is defined

if(NOT TARGET gtest_main)
    include(FetchContent)

    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_PROGRESS TRUE
        GIT_SHALLOW TRUE
        GIT_TAG v1.14.0
        SYSTEM)

    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    set_target_properties(gtest PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # Suppress warnings from googletest source compilation
    target_compile_options(gtest PRIVATE -w)
    target_compile_options(gtest_main PRIVATE -w)

    include(GoogleTest)
endif()

# On Windows shared-library builds, gtest_discover_tests runs the test
# executable during the build to enumerate test names. The executable needs
# its DLL dependencies in the same directory to load. Call this function after
# defining a test target to add a POST_BUILD copy of all transitive DLLs.
if(NOT COMMAND tanh_copy_dlls_for_tests)
    function(tanh_copy_dlls_for_tests target)
        if(WIN32 AND BUILD_SHARED_LIBS)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${target}>
                    $<TARGET_FILE_DIR:${target}>
                COMMAND_EXPAND_LISTS
            )
        endif()
    endfunction()
endif()
