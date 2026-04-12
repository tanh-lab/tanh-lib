set(PROJECT_NAME test_state)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_StateCore.cpp
	test_StateNotifications.cpp
	test_ParameterDefinitions.cpp
	test_StateSerialization.cpp
	test_ParameterHandle.cpp
	test_StateThreadSafety.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		tanh::State
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

# Benchmark target
add_executable(benchmark_state
	benchmark_State.cpp
)

target_link_libraries(benchmark_state
	PRIVATE
		tanh::State
		tanh::Core
		benchmark::benchmark
		gtest
)