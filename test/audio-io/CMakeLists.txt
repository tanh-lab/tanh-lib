set(PROJECT_NAME test_audio_io)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_AudioIO.cpp
	test_Recorder.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		tanh::AudioIO
		tanh::Core
		gtest_main
		gtest
)

# The recorder with the schedule that puts its takes on bar lines: needs the transport.
if(TARGET tanh::DSP)
	target_sources(${PROJECT_NAME} PRIVATE test_RecordedTake.cpp)
	target_link_libraries(${PROJECT_NAME} PRIVATE tanh::DSP)
endif()

tanh_copy_runtime_dlls(${PROJECT_NAME})

if(CMAKE_CROSSCOMPILING)
	gtest_add_tests(TARGET ${PROJECT_NAME})
else()
	gtest_discover_tests(${PROJECT_NAME})
endif()
