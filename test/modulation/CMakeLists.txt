set(PROJECT_NAME test_modulation)
project(${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_LFOSource.cpp
	test_ModulationMatrixCore.cpp
	test_CyclicModulation.cpp
	test_BaseProcessor.cpp
	test_SmartHandle.cpp
	test_ResolvedTarget.cpp
	test_Integration.cpp
	test_PolyphonicModulation.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		tanh::Modulation
		tanh::DSP
		tanh::State
		tanh::Core
		gtest_main
		gtest
)

include(GoogleTest)
if(TANH_OPERATING_SYSTEM STREQUAL "iOS")
	gtest_add_tests(TARGET ${PROJECT_NAME})
else()
	gtest_discover_tests(${PROJECT_NAME})
endif()

# Benchmark target
add_executable(benchmark_modulation
	benchmark_ModulationMatrix.cpp
)

target_link_libraries(benchmark_modulation
	PRIVATE
		tanh::Modulation
		tanh::DSP
		tanh::State
		tanh::Core
		benchmark::benchmark
		gtest
)
