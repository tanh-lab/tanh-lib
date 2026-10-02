set(PROJECT_NAME test_modulation)
project(${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

# Tests that need no thl::State (Modulation can be built without it).
target_sources(${PROJECT_NAME} PRIVATE
	test_ParameterBackend.cpp
	test_ResolvedTarget.cpp
	test_XYPad.cpp
	test_XYPadMatrix.cpp
	test_RoutingEnabled.cpp
	test_MotionLane.cpp
	test_MotionRecorder.cpp
	test_MotionRecorderClock.cpp
	test_MotionRecorderThreads.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		tanh::Modulation
		tanh::Core
		gtest_main
		gtest
)

if(TARGET tanh::State)
	target_sources(${PROJECT_NAME} PRIVATE
		test_LFOSource.cpp
		test_ModulationMatrixCore.cpp
		test_CyclicModulation.cpp
		test_BaseProcessor.cpp
		test_SmartHandle.cpp
		test_StateParameterBackend.cpp
		test_Integration.cpp
		test_PolyphonicModulation.cpp
		test_InputEventQueue.cpp
		test_ConcurrentRebuild.cpp
		test_ModulationScope.cpp
		test_DspFixture_Wavefolder.cpp
		test_DspFixture_Limiter.cpp
		test_DspFixture_Reverb.cpp
	)
	target_link_libraries(${PROJECT_NAME} PRIVATE tanh::DSP tanh::State)
endif()

include(GoogleTest)
tanh_copy_runtime_dlls(${PROJECT_NAME})

if(CMAKE_CROSSCOMPILING)
	gtest_add_tests(TARGET ${PROJECT_NAME})
else()
	gtest_discover_tests(${PROJECT_NAME})
endif()

# Benchmark target
if(TARGET tanh::State)
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
endif()
