set(PROJECT_NAME test_transport)
project(${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
    test_InternalTransportClock.cpp
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        tanh::DSP
        gtest_main
        gtest
)

if(TANH_OPERATING_SYSTEM STREQUAL "iOS")
    gtest_add_tests(TARGET ${PROJECT_NAME})
else()
    gtest_discover_tests(${PROJECT_NAME})
endif()
