set(PROJECT_NAME test_core)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_RCU.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		tanh::Core
		gtest_main
		gtest
)

tanh_copy_dlls_for_tests(${PROJECT_NAME})

if(CMAKE_CROSSCOMPILING)
	gtest_add_tests(TARGET ${PROJECT_NAME})
else()
	gtest_discover_tests(${PROJECT_NAME})
endif()