set(PROJECT_NAME test_link)
project(${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

# Real Link SDK backends (TANH_BUILD_LINK). The fake-backend clock tests live in
# test/dsp and run without the option.
target_sources(${PROJECT_NAME} PRIVATE
	test_LinkSession.cpp
	test_LinkAudioCodec.cpp
)

# The wire-format codec and the sharing helpers are header-only and private to
# tanh::Link (src/link): the codec test includes them directly, nothing is exported.
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/link)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
		tanh::Link
		tanh::DSP
		tanh::Core
		gtest_main
		gtest
)

tanh_copy_runtime_dlls(${PROJECT_NAME})

# Every test that enables Link joins the network: never run two at once (ctest -j),
# or they would discover each other.
if(CMAKE_CROSSCOMPILING)
	gtest_add_tests(TARGET ${PROJECT_NAME})
else()
	gtest_discover_tests(${PROJECT_NAME} PROPERTIES RESOURCE_LOCK link-network)
endif()

# TEST-PLAN cases against a second, raw Link SDK peer in the same process (desktop
# only: Ableton::Link is the C++ SDK target). Real network, so they carry the label
# "link-peers", which every test preset excludes (CMakePresets.json); run them
# without a preset: `ctest --test-dir <build> -L link-peers`.
if(TARGET Ableton::Link)
	add_executable(test_link_peers)
	target_sources(test_link_peers PRIVATE
		test_LinkPeers.cpp
	)
	target_link_libraries(test_link_peers
		PRIVATE
			tanh::Link
			tanh::DSP
			tanh::Core
			Ableton::Link
			gtest_main
			gtest
	)
	tanh_copy_runtime_dlls(test_link_peers)
	if(TANH_OPERATING_SYSTEM STREQUAL "Windows")
		link_pin_win32_winnt(test_link_peers)
	endif()
	if(CMAKE_CROSSCOMPILING)
		gtest_add_tests(TARGET test_link_peers)
	else()
		gtest_discover_tests(test_link_peers
			PROPERTIES LABELS link-peers RESOURCE_LOCK link-network TIMEOUT 120)
	endif()
endif()
