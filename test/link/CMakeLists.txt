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

# Every test that enables Link joins the network: never run two at once (ctest -j),
# or they would discover each other.
if(CMAKE_CROSSCOMPILING)
    gtest_add_tests(TARGET ${PROJECT_NAME})
else()
    gtest_discover_tests(${PROJECT_NAME} PROPERTIES RESOURCE_LOCK link-network)
endif()

# TEST-PLAN cases against a second, raw Link SDK peer in the same process (desktop
# only: Ableton::Link is the C++ SDK target). Real network: label "link-peers",
# excluded in CI with `ctest -LE link-peers`.
if(TARGET Ableton::Link)
    add_executable(test_link_peers test_LinkPeers.cpp)
    target_link_libraries(test_link_peers
        PRIVATE
            tanh::Link
            tanh::DSP
            tanh::Core
            Ableton::Link
            gtest_main
            gtest
    )
    tanh_copy_runtime_dlls(test_link_peers)
    if(TANH_OPERATING_SYSTEM STREQUAL "Windows")
        link_pin_win32_winnt(test_link_peers)
    endif()
    gtest_discover_tests(test_link_peers
        PROPERTIES LABELS link-peers RESOURCE_LOCK link-network TIMEOUT 120)
endif()
