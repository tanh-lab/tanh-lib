set(PROJECT_NAME test_link)
project(${PROJECT_NAME} VERSION ${PROJECT_VERSION})

# Real Link SDK backends (TANH_WITH_LINK). The fake-backend clock tests live in
# test/dsp and run without the option.
add_executable(${PROJECT_NAME} test_LinkSession.cpp)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        tanh::Link
        tanh::DSP
        tanh::Core
        gtest_main
        gtest
)

include(GoogleTest)
tanh_copy_runtime_dlls(${PROJECT_NAME})

if(CMAKE_CROSSCOMPILING)
    gtest_add_tests(TARGET ${PROJECT_NAME})
else()
    gtest_discover_tests(${PROJECT_NAME})
endif()
