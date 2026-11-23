set(PROJECT_NAME test_midi)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_Ump.cpp
	test_Midi1StreamParser.cpp
	test_Midi1Converter.cpp
	test_InputQueue.cpp
	test_DeviceManager.cpp
	test_PortReader.cpp
	test_StreamClock.cpp
	test_BlockEventCollector.cpp
)

# AudioMidiCallback is an AudioIODeviceCallback: its tests need tanh::AudioIO as well.
if(TARGET tanh::AudioIO)
	target_sources(${PROJECT_NAME} PRIVATE test_AudioMidiCallback.cpp)
	target_link_libraries(${PROJECT_NAME} PRIVATE tanh::AudioIO)
	target_compile_definitions(${PROJECT_NAME} PRIVATE THL_TEST_WITH_AUDIO_IO=1)
endif()

# PortReader is the platform-independent part of the Android backend, a private header.
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src/midi)

# DeviceManager against the real CoreMIDI, through virtual ports of the test process:
# on macOS, and on iOS in the simulator's test bundle.
if(TANH_OPERATING_SYSTEM STREQUAL "macOS" OR TANH_OPERATING_SYSTEM STREQUAL "iOS")
	target_sources(${PROJECT_NAME} PRIVATE test_DeviceManager_coremidi.cpp)
	target_link_libraries(${PROJECT_NAME} PRIVATE
		"-framework CoreMIDI" "-framework CoreFoundation")
endif()

# iOS refuses virtual MIDI ports to an app that does not declare the audio background
# mode, so the test bundle gets an Info.plist of its own that does.
if(TANH_OPERATING_SYSTEM STREQUAL "iOS")
	set_target_properties(${PROJECT_NAME} PROPERTIES
		MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in)
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
