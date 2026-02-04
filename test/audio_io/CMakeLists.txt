set(PROJECT_NAME test_audio_io)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_AudioIO.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		tanh::AudioIO
		tanh::Core
		gtest_main
		gtest
)

gtest_discover_tests(${PROJECT_NAME})
