set(PROJECT_NAME test_midi)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_Ump.cpp
	test_Midi1StreamParser.cpp
	test_Midi1Converter.cpp
	test_InputQueue.cpp
	test_DeviceManager.cpp
)

# DeviceManager against the real CoreMIDI, through virtual ports of the test process.
# macOS only: the iOS test bundles run in a simulator whose MIDI service is not ours to
# rely on.
if(TANH_OPERATING_SYSTEM STREQUAL "macOS")
	target_sources(${PROJECT_NAME} PRIVATE test_DeviceManager_coremidi.cpp)
	target_link_libraries(${PROJECT_NAME} PRIVATE
		"-framework CoreMIDI" "-framework CoreFoundation")
endif()

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		tanh::Midi
		gtest_main
		gtest
)

tanh_copy_runtime_dlls(${PROJECT_NAME})

if(CMAKE_CROSSCOMPILING)
	gtest_add_tests(TARGET ${PROJECT_NAME})
else()
	gtest_discover_tests(${PROJECT_NAME})
endif()
