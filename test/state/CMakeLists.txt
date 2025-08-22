set(PROJECT_NAME test_state)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_State.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		tanh::State
		tanh::Core
		gtest_main
		gtest
)

gtest_discover_tests(${PROJECT_NAME})