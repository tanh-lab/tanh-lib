set(PROJECT_NAME test_dsp)
project (${PROJECT_NAME} VERSION ${PROJECT_VERSION})

add_executable(${PROJECT_NAME})

target_sources(${PROJECT_NAME} PRIVATE
	test_AudioBuffer.cpp
	test_Limiter.cpp
)

if(TARGET tanh_resonator_core)
	target_sources(${PROJECT_NAME} PRIVATE
		test_RingsResonatorCore.cpp
	)

	# Generator tool for producing reference fixture data
	add_executable(generate_rings_reference generate_rings_reference.cpp)
	target_link_libraries(generate_rings_reference
		PRIVATE
			tanh::ResonatorCore
			tanh::DSP
			tanh::Core
	)
	target_compile_definitions(generate_rings_reference
		PRIVATE
			FIXTURE_OUTPUT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
	)

	# Embed reference fixtures (only if they exist)
	include(${CMAKE_SOURCE_DIR}/cmake/binary_data.cmake)
	set(_fixtures_dir "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
	if(EXISTS "${_fixtures_dir}/modal.bin")
		tanh_add_binary_data(rings_test_fixtures
			NAMESPACE RingsTestFixtures
			HEADER_NAME RingsTestFixtures.h
			SOURCES
				${_fixtures_dir}/modal.bin
				${_fixtures_dir}/sympathetic_string.bin
				${_fixtures_dir}/modulated_string.bin
				${_fixtures_dir}/fm_voice.bin
				${_fixtures_dir}/sympathetic_string_quantized.bin
				${_fixtures_dir}/string_and_reverb.bin
		)
		target_link_libraries(${PROJECT_NAME} PRIVATE rings_test_fixtures)
		target_compile_definitions(${PROJECT_NAME} PRIVATE RINGS_HAS_REFERENCE_FIXTURES)
	endif()
endif()

target_link_libraries(${PROJECT_NAME}
	PRIVATE
			tanh::DSP
			tanh::Core
			gtest_main
			gtest
	)

if(TARGET tanh_resonator_core)
	target_link_libraries(${PROJECT_NAME}
		PRIVATE
			tanh::ResonatorCore
	)
endif()

gtest_discover_tests(${PROJECT_NAME})
