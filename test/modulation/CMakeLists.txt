set(PROJECT_NAME test_modulation)
project(${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_ModulationMatrix.cpp
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
gtest_discover_tests(${PROJECT_NAME})
