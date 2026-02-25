cmake_minimum_required(VERSION 3.15)

# ==============================================================================
# Define the options for the tanh library
# ==============================================================================

# Shall the library be built as a shared library?
option(BUILD_SHARED_LIBS "Build the library as a shared library" ON)
option(TANH_WITH_INSTALL "Add install targets" ON)
option(TANH_WITH_TESTS "Add Build Tests" ON)
option(TANH_WITH_DOCS "Add Build Documentation" ON)

option(TANH_WITH_RTSAN "Enable RealtimeSanitizer (rtan) checks (requires clang 20)" OFF)

# Define available components
set(TANH_COMPONENTS Core State DSP AudioIO)

# Option to control which components to build
option(TANH_BUILD_CORE "Build Core component" ON)
option(TANH_BUILD_STATE "Build State component" ON)
option(TANH_BUILD_DSP "Build DSP component" ON)
option(TANH_BUILD_AUDIO_IO "Build AudioIO component" ON)
option(TANH_BUILD_RESONATOR "Build Resonator component" ON)
option(TANH_BUILD_RESONATOR_WRAPPER "Build RingsResonator wrapper (requires libsamplerate)" ON)

# ==============================================================================
# Get project version
# ==============================================================================

# Set a default version if not in git or git not available
set(PROJECT_VERSION_SHORT "0.0.0")
set(PROJECT_VERSION_FULL "0.0.0")

# Try to get version from git if available
execute_process(COMMAND git describe --tags --abbrev=0
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE GIT_VERSION_SHORT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)

if(GIT_VERSION_SHORT)
    execute_process(COMMAND git describe --dirty
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE GIT_VERSION_FULL
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    
    # Remove the 'v' prefix if present
    if(GIT_VERSION_SHORT MATCHES "^v(.+)$")
        string(SUBSTRING ${GIT_VERSION_SHORT} 1 -1 PROJECT_VERSION_SHORT)
    else()
        set(PROJECT_VERSION_SHORT ${GIT_VERSION_SHORT})
    endif()
    
    if(GIT_VERSION_FULL MATCHES "^v(.+)$")
        string(SUBSTRING ${GIT_VERSION_FULL} 1 -1 PROJECT_VERSION_FULL)
    else()
        set(PROJECT_VERSION_FULL ${GIT_VERSION_FULL})
    endif()
endif()

# ==============================================================================
# Setup the project
# ==============================================================================

set (PROJECT_NAME tanh)

project(${PROJECT_NAME} VERSION ${PROJECT_VERSION_SHORT})

if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(TANH_OPERATING_SYSTEM "macOS")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(TANH_OPERATING_SYSTEM "iOS")
    else()
        set(TANH_OPERATING_SYSTEM "Unknown Apple OS")
    endif()
elseif(UNIX)
    set(TANH_OPERATING_SYSTEM "Linux")
elseif(WIN32)
    set(TANH_OPERATING_SYSTEM "Windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(TANH_OPERATING_SYSTEM "Android")
else()
    set(TANH_OPERATING_SYSTEM "Unknown")
endif()

message(STATUS "Project version: ${PROJECT_VERSION_SHORT} (${PROJECT_VERSION_FULL})")

# Sets the minimum macOS/iOS version, c++20 is only available from macOS 11.0
if (APPLE)
	# For iOS builds, use 14.0 as minimum (required for SwiftUI @main)
	# For macOS builds, use 12.0 as minimum
	if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
		if(NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET LESS "14.0")
			set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum iOS deployment version")
		endif()
		message(STATUS "The minimum iOS version is set to ${CMAKE_OSX_DEPLOYMENT_TARGET}")
	else()
		if(NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET LESS "12.0")
			set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)
		endif()
		message(STATUS "The minimum macOS version is set to ${CMAKE_OSX_DEPLOYMENT_TARGET}")
	endif()
endif ()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

## ==============================================================================
# Fetch dependencies
# ==============================================================================

include(FetchContent)

if(TANH_BUILD_CORE)
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "Build spdlog shared library" FORCE)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Build spdlog examples" FORCE)
    set(SPDLOG_BUILD_EXAMPLES OFF CACHE BOOL "Build spdlog examples" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "Build spdlog tests" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "Build spdlog bench" FORCE)

    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.14.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(spdlog)
endif()

# ==============================================================================
# Build the library with components
# ==============================================================================

# Track which components are being built
set(TANH_BUILT_COMPONENTS "")

# Core component (always needed by others)
if(TANH_BUILD_CORE)
    add_library(${PROJECT_NAME}_core
        src/core.cpp
        src/core/Dispatcher.cpp
        src/core/Logger.cpp
    )
    
    # enable position independent code because otherwise the static library cannot be linked into a shared library
    set_target_properties(${PROJECT_NAME}_core PROPERTIES 
        POSITION_INDEPENDENT_CODE ON 
        VERSION ${PROJECT_VERSION} 
        SOVERSION ${PROJECT_VERSION_MAJOR})

    # add an alias so that the project can be used with add_subdirectory
    add_library(${PROJECT_NAME}::Core ALIAS ${PROJECT_NAME}_core)

    target_include_directories(${PROJECT_NAME}_core
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    target_compile_definitions(${PROJECT_NAME}_core
        PUBLIC
        # Version number
        -DTANH_VERSION="${PROJECT_VERSION_FULL}"
    )
    target_compile_definitions(${PROJECT_NAME}_core
        PUBLIC
        $<$<CONFIG:Release>:THL_LOG_COMPILED_MAX_LEVEL=1>
        $<$<NOT:$<CONFIG:Release>>:THL_LOG_COMPILED_MAX_LEVEL=4>
    )

    target_link_libraries(${PROJECT_NAME}_core
        PRIVATE
        spdlog::spdlog_header_only
    )

    list(APPEND TANH_BUILT_COMPONENTS ${PROJECT_NAME}_core)
endif()

# State component (depends on Core)
if(TANH_BUILD_STATE)
    if(NOT TARGET ${PROJECT_NAME}_core)
        message(FATAL_ERROR "State component requires Core component")
    endif()

    # Enable installation for nlohmann_json so it can be exported with tanh_state
    # set(JSON_Install ON CACHE BOOL "Install nlohmann_json headers" FORCE)
    
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
    
    add_library(${PROJECT_NAME}_state
        src/state/Parameter.cpp
        src/state/StateGroup.cpp
        src/state/State.cpp
    )
    
    set_target_properties(${PROJECT_NAME}_state PROPERTIES 
        POSITION_INDEPENDENT_CODE ON 
        VERSION ${PROJECT_VERSION} 
        SOVERSION ${PROJECT_VERSION_MAJOR})
    
    add_library(${PROJECT_NAME}::State ALIAS ${PROJECT_NAME}_state)
    
    target_include_directories(${PROJECT_NAME}_state
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    
    target_link_libraries(${PROJECT_NAME}_state 
        PUBLIC 
            ${PROJECT_NAME}_core 
            nlohmann_json::nlohmann_json
    )

    target_compile_definitions(${PROJECT_NAME}_state
        PUBLIC
        # Set TANH_STATE_ENABLED to enable state features
        -DTANH_STATE_ENABLED
    )

    list(APPEND TANH_BUILT_COMPONENTS ${PROJECT_NAME}_state)
endif()

# DSP component (depends on Core)
if(TANH_BUILD_DSP)
    if(NOT TARGET ${PROJECT_NAME}_core)
        message(FATAL_ERROR "DSP component requires Core component")
    endif()

    add_library(${PROJECT_NAME}_dsp
        src/dsp/utils/ADSR.cpp
        src/dsp/utils/DspMath.cpp
        src/dsp/utils/HannWindow.cpp
        src/dsp/utils/Scales.cpp
        src/dsp/utils/SmoothedValue.cpp
        src/dsp/utils/Limiter.cpp
        src/dsp/utils/Random.cpp

        src/dsp/synth/SineProcessor.cpp
        src/dsp/granular/GrainProcessor.cpp
    )

    set_target_properties(${PROJECT_NAME}_dsp PROPERTIES 
        POSITION_INDEPENDENT_CODE ON 
        VERSION ${PROJECT_VERSION} 
        SOVERSION ${PROJECT_VERSION_MAJOR})

    add_library(${PROJECT_NAME}::DSP ALIAS ${PROJECT_NAME}_dsp)

    target_include_directories(${PROJECT_NAME}_dsp
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    target_link_libraries(${PROJECT_NAME}_dsp PUBLIC ${PROJECT_NAME}_core)

    target_compile_definitions(${PROJECT_NAME}_dsp
        PUBLIC
        # Set TANH_DSP_ENABLED to enable DSP features
        -DTANH_DSP_ENABLED
    )

    list(APPEND TANH_BUILT_COMPONENTS ${PROJECT_NAME}_dsp)
endif()

# Resonator Core component (depends on DSP)
if(TANH_BUILD_RESONATOR)
    if(NOT TARGET ${PROJECT_NAME}_dsp)
        message(FATAL_ERROR "Resonator component requires DSP component")
    endif()

    add_library(${PROJECT_NAME}_resonator_core
        src/dsp/resonator/rings/Part.cpp
        src/dsp/resonator/rings/Resonator.cpp
        src/dsp/resonator/rings/String.cpp
        src/dsp/resonator/rings/FmVoice.cpp
        src/dsp/resonator/rings/StringSynthPart.cpp
        src/dsp/resonator/rings/Resources.cpp
    )

    set_target_properties(${PROJECT_NAME}_resonator_core PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})

    add_library(${PROJECT_NAME}::ResonatorCore ALIAS ${PROJECT_NAME}_resonator_core)

    target_include_directories(${PROJECT_NAME}_resonator_core
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    target_link_libraries(${PROJECT_NAME}_resonator_core
        PUBLIC
        ${PROJECT_NAME}_dsp
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${PROJECT_NAME}_resonator_core PRIVATE
            -Wno-unused-local-typedefs
            -Wno-unused-parameter
            -Wno-sign-compare
            -Wno-unused-variable
            -Wno-c++11-narrowing
        )
    endif()

    list(APPEND TANH_BUILT_COMPONENTS ${PROJECT_NAME}_resonator_core)
endif()

if(TANH_BUILD_RESONATOR AND TANH_BUILD_RESONATOR_WRAPPER)
    if(NOT TARGET ${PROJECT_NAME}_resonator_core)
        message(FATAL_ERROR "Resonator wrapper requires Resonator core component")
    endif()

    find_path(TANH_SAMPLERATE_INCLUDE_DIR samplerate.h)
    find_library(TANH_SAMPLERATE_LIBRARY NAMES samplerate)

    if(TANH_SAMPLERATE_INCLUDE_DIR AND TANH_SAMPLERATE_LIBRARY)
        add_library(${PROJECT_NAME}_resonator
            src/dsp/resonator/RingsResonator.cpp
            src/dsp/resonator/SampleRateConverter.cpp
        )

        set_target_properties(${PROJECT_NAME}_resonator PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR})

        add_library(${PROJECT_NAME}::Resonator ALIAS ${PROJECT_NAME}_resonator)

        target_include_directories(${PROJECT_NAME}_resonator
            PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
            PRIVATE
            ${TANH_SAMPLERATE_INCLUDE_DIR}
        )

        target_link_libraries(${PROJECT_NAME}_resonator
            PUBLIC
            ${PROJECT_NAME}_resonator_core
            PRIVATE
            ${TANH_SAMPLERATE_LIBRARY}
        )

        list(APPEND TANH_BUILT_COMPONENTS ${PROJECT_NAME}_resonator)
    else()
        message(WARNING "libsamplerate not found; skipping tanh_resonator wrapper target")
    endif()
endif()

# AudioIO component (depends on Core)
if(TANH_BUILD_AUDIO_IO)
    if(NOT TARGET ${PROJECT_NAME}_core)
        message(FATAL_ERROR "AudioIO component requires Core component")
    endif()

    FetchContent_Declare(
        miniaudio
        GIT_REPOSITORY https://github.com/mackron/miniaudio.git
        GIT_TAG 0.11.24
        GIT_SHALLOW TRUE
    )
    set(MINIAUDIO_NO_EXTRA_NODES ON)
    FetchContent_MakeAvailable(miniaudio)

    # Disable code signing for miniaudio targets on iOS
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        get_property(miniaudio_targets DIRECTORY ${miniaudio_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target ${miniaudio_targets})
            if(TARGET ${target})
                set_target_properties(${target} PROPERTIES
                    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
                    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
                    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
                )
            endif()
        endforeach()
    endif()


    add_library(${PROJECT_NAME}_audio_io
        src/audio_io/AudioDeviceManager.cpp
        src/audio_io/AudioFileLoader.cpp
        src/audio_io/AudioFileSink.cpp
        src/audio_io/AudioPlayerSource.cpp
        src/audio_io/DataSource.cpp
        src/audio_io/miniaudio_impl.cpp
    )

    # On Apple platforms, miniaudio implementation includes Objective-C code
    # for CoreAudio/AVFoundation, so it must be compiled as Objective-C++
    if(APPLE)
        set_source_files_properties(
            src/audio_io/miniaudio_impl.cpp
            PROPERTIES COMPILE_FLAGS "-x objective-c++"
        )
    endif()

    set_target_properties(${PROJECT_NAME}_audio_io PROPERTIES 
        POSITION_INDEPENDENT_CODE ON 
        VERSION ${PROJECT_VERSION} 
        SOVERSION ${PROJECT_VERSION_MAJOR})

    add_library(${PROJECT_NAME}::AudioIO ALIAS ${PROJECT_NAME}_audio_io)

    target_include_directories(${PROJECT_NAME}_audio_io
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        ${miniaudio_SOURCE_DIR}
    )

    target_link_libraries(${PROJECT_NAME}_audio_io PUBLIC ${PROJECT_NAME}_core)

    # Link platform-specific frameworks required by miniaudio
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_link_libraries(${PROJECT_NAME}_audio_io PUBLIC
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework CoreMIDI"
            "-framework Accelerate"
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_link_libraries(${PROJECT_NAME}_audio_io PUBLIC
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework CoreFoundation"
        )
    endif()

    target_compile_definitions(${PROJECT_NAME}_audio_io
        PUBLIC
        # Set TANH_AUDIO_IO_ENABLED to enable AudioIO features
        -DTANH_AUDIO_IO_ENABLED
    )

    list(APPEND TANH_BUILT_COMPONENTS ${PROJECT_NAME}_audio_io)
endif()

# Apply common settings to all components

foreach(target ${TANH_BUILT_COMPONENTS})
    # Disable code signing for iOS library targets (only apps need signing)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set_target_properties(${target} PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
            XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        )
    endif()

    if(TANH_WITH_RTSAN)
        include(cmake/real-time-sanitizers.cmake)
        tanh_rtsan_configure(${target})
    endif()

    target_compile_options(${target}
        PUBLIC
            # -fsanitize=address
            # -fsanitize=leak
            # -fsanitize=realtime
            # -fsanitize=undefined
            # -fsanitize=thread
    )
    target_link_options(${target}
        PUBLIC
            # -fsanitize=address
            # -fsanitize=leak
            # -fsanitize=realtime
            # -fsanitize=undefined
            # -fsanitize=thread
    )
endforeach()

# ==============================================================================
# Add install targets for the library
# ==============================================================================

#if(TANH_WITH_INSTALL)
#    include(cmake/install.cmake)
#    include(cmake/package.cmake)
#endif()

# ==============================================================================
# Add tests, examples, and documentation
# ==============================================================================

if (TANH_WITH_TESTS)
    include(cmake/test-deps.cmake)
    if (TANH_BUILD_CORE)
        add_subdirectory(test/core)
    endif()
    if (TANH_BUILD_STATE)
        add_subdirectory(test/state)
    endif()
    if (TANH_BUILD_DSP)
        add_subdirectory(test/dsp)
    endif()
    if (TANH_BUILD_AUDIO_IO)
        add_subdirectory(test/audio_io)
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(examples/ios/audio_io)
endif()


# if (TANH_WITH_DOCS)
#     add_subdirectory(docs)
# endif()
