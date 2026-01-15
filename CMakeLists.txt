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
set(TANH_COMPONENTS Core State DSP)

# Option to control which components to build
option(TANH_BUILD_CORE "Build Core component" ON)
option(TANH_BUILD_STATE "Build State component" ON)
option(TANH_BUILD_DSP "Build DSP component" ON)

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

## ==============================================================================
# Fetch dependencies
# ==============================================================================

include(FetchContent)

# ==============================================================================
# Build the library with components
# ==============================================================================

# Track which components are being built
set(TANH_BUILT_COMPONENTS "")

# Core component (always needed by others)
if(TANH_BUILD_CORE)
    add_library(${PROJECT_NAME}_core
        src/core.cpp
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

    target_link_libraries(${PROJECT_NAME}_core)

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
        src/dsp/utils/HannWindow.cpp
        src/dsp/utils/Scales.cpp
        src/dsp/utils/SmoothedValue.cpp

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
        # Link header only library choc
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/modules/choc>
    )

    target_link_libraries(${PROJECT_NAME}_dsp PUBLIC ${PROJECT_NAME}_core)

    target_compile_definitions(${PROJECT_NAME}_dsp
        PUBLIC
        # Set TANH_DSP_ENABLED to enable DSP features
        -DTANH_DSP_ENABLED
    )

    list(APPEND TANH_BUILT_COMPONENTS ${PROJECT_NAME}_dsp)
endif()

# Apply common settings to all components

foreach(target ${TANH_BUILT_COMPONENTS})
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
        # add_subdirectory(test/dsp) # TODO: add dsp tests
    endif()
endif()

# if(TANH_WITH_EXAMPLES)
#     add_subdirectory(examples)
# endif()

# if (TANH_WITH_DOCS)
#     add_subdirectory(docs)
# endif()
