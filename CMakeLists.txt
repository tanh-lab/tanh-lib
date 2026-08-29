cmake_minimum_required(VERSION 3.25)

# ==============================================================================
# Define the options for the tanh library
# ==============================================================================

# Shall the library be built as a shared library?
option(BUILD_SHARED_LIBS "Build the library as a shared library" ON)
option(TANH_WITH_INSTALL "Add install targets" ON)
option(TANH_WITH_PACKAGING "Add CPack packaging targets (top-level builds only)" OFF)
option(TANH_WITH_TESTS "Add Build Tests" ON)
option(TANH_WITH_EXAMPLES "Add Build Examples" ON)
option(TANH_WITH_DOCS "Add Build Documentation" ON)

option(TANH_WITH_RTSAN "Enable RealtimeSanitizer (rtan) checks (requires clang 20)" OFF)
option(TANH_WITH_ASAN "Enable AddressSanitizer (asan) checks" OFF)
option(TANH_WITH_USAN "Enable UndefinedBehaviorSanitizer (usan) checks" OFF)
option(TANH_WITH_TSAN "Enable ThreadSanitizer (tsan) checks" OFF)
option(TANH_WITH_MSAN "Enable MemorySanitizer (msan) checks" OFF)
option(TANH_WITH_LSAN "Enable LeakSanitizer (lsan) checks" OFF)

# Platform log sinks. The core component links nothing platform-specific by
# default so that permissively licensed consumers (e.g. anira) can embed
# tanh::Core without pulling in system libraries. On Linux, the journald sink
# is opt-in: it links libsystemd (found via pkg-config) and routes the platform
# sink through sd_journal_send(); without it, Linux logs go to stdout/stderr.
option(TANH_WITH_JOURNALD "Linux: send platform logs to systemd-journald (links libsystemd)" OFF)

# Define available components
set(TANH_COMPONENTS Core State DSP AudioIO)

# Option to control which components to build
option(TANH_BUILD_CORE "Build Core component" ON)
option(TANH_BUILD_STATE "Build State component" ON)
option(TANH_BUILD_DSP "Build DSP component" ON)
option(TANH_BUILD_MODULATION "Build Modulation component" ON)
option(TANH_BUILD_AUDIO_IO "Build AudioIO component" ON)

# ==============================================================================
# Get project version
# ==============================================================================

# Version from git (cmake/tanh/git-version.cmake): numeric part of the nearest tag
# for CMake, the full `git describe` for humans; "0.0.0+g<hash>" without a tag.
include(cmake/tanh/git-version.cmake)
tanh_git_version(${CMAKE_CURRENT_SOURCE_DIR})
set(PROJECT_VERSION_SHORT "${TANH_VERSION_SHORT}")
set(PROJECT_VERSION_FULL "${TANH_VERSION_FULL}")

# ==============================================================================
# Setup the project
# ==============================================================================

set (PROJECT_NAME tanh)

project(${PROJECT_NAME} VERSION ${PROJECT_VERSION_SHORT})

# The shared CMake modules (cmake/tanh/, installed verbatim from a pinned tanh-tooling
# release — never edit them here). TANH_CMAKE_DIR is kept for downstream projects
# that include our helpers by path (cosmos-backend: gtest-deps.cmake forwarder).
set(TANH_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake" CACHE INTERNAL
    "Path to tanh-lib's CMake helpers")

# Platform axes: TANH_OPERATING_SYSTEM, TANH_BINARY_FORMAT, TANH_IOS_PLATFORM and the
# THL_PLATFORM_* definitions tanh::Core exposes. The iOS toolchain sets
# TANH_IOS_PLATFORM; the former spelling THL_IOS_PLATFORM is still honoured.
if(DEFINED THL_IOS_PLATFORM AND NOT DEFINED TANH_IOS_PLATFORM)
    set(TANH_IOS_PLATFORM "${THL_IOS_PLATFORM}")
endif()
include(cmake/tanh/platform.cmake)

message(STATUS "Project version: ${PROJECT_VERSION_SHORT} (${PROJECT_VERSION_FULL})")

# CMAKE_INSTALL_INCLUDEDIR & co. are referenced in the targets' install
# interfaces below, so they must be defined before the first add_library().
include(GNUInstallDirs)

# macOS 12.0 / iOS 14.0 minimum (C++20 needs macOS >= 11; SwiftUI @main needs iOS >= 14).
# Only a default when nobody set one — never FORCE'd, so that as a sub-project the
# embedding project's own minimum is left alone.
include(cmake/tanh/apple.cmake)
tanh_apple_deployment_target(MACOS 12.0 IOS 14.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Non-Apple clang on macOS gets no sysroot from CMake; without it the compile
# database lacks -isysroot and clang-tidy cannot find system headers.
tanh_apple_sysroot_from_xcrun()

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
        src/core/Dispatcher.cpp
        src/core/Logger.cpp
        src/core/Thread.cpp
        src/core/RCU.cpp
    )
    
    set_target_properties(${PROJECT_NAME}_core PROPERTIES
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

    # Platform compile definitions (THL_PLATFORM_<OS>=1, THL_PLATFORM_IOS_<DEVICE|SIMULATOR>=1)
    target_compile_definitions(${PROJECT_NAME}_core PUBLIC ${TANH_PLATFORM_COMPILE_DEFINITIONS})

    # Build type definitions
    target_compile_definitions(${PROJECT_NAME}_core
        PUBLIC
        $<$<CONFIG:Debug>:THL_DEBUG=1>
        $<$<CONFIG:Release>:THL_RELEASE=1>
        $<$<CONFIG:RelWithDebInfo>:THL_RELWITHDEBINFO=1>
        $<$<CONFIG:MinSizeRel>:THL_MINSIZEREL=1>
    )

    if(TANH_OPERATING_SYSTEM STREQUAL "Android")
        target_link_libraries(${PROJECT_NAME}_core PRIVATE log)
    elseif(TANH_OPERATING_SYSTEM STREQUAL "Linux" AND TANH_WITH_JOURNALD)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
        target_link_libraries(${PROJECT_NAME}_core PRIVATE PkgConfig::SYSTEMD)
        target_compile_definitions(${PROJECT_NAME}_core PRIVATE THL_WITH_JOURNALD=1)
    endif()

    list(APPEND TANH_BUILT_COMPONENTS ${PROJECT_NAME}_core)
endif()

# State component (depends on Core)
if(TANH_BUILD_STATE)
    if(NOT TARGET ${PROJECT_NAME}_core)
        message(FATAL_ERROR "State component requires Core component")
    endif()

    # nlohmann_json is a PUBLIC dependency of State: when tanh installs, install
    # its headers + CMake package too so tanhConfig.cmake's
    # find_dependency(nlohmann_json) resolves for consumers.
    if(TANH_WITH_INSTALL)
        set(JSON_Install ON CACHE BOOL "Install nlohmann_json headers" FORCE)
    endif()
    
    # Fetch the release tarball instead of cloning the repo — the full git
    # history is ~150MB while the release archive is a few MB, and the tarball
    # is content-addressable so CMake will reuse a cached copy across build
    # folders.
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        SYSTEM
    )
    FetchContent_MakeAvailable(nlohmann_json)
    
    add_library(${PROJECT_NAME}_state
        src/state/Parameter.cpp
        src/state/StateGroup.cpp
        src/state/State.cpp
    )
    
    set_target_properties(${PROJECT_NAME}_state PROPERTIES 
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
        src/dsp/BaseProcessor.cpp
        src/dsp/utils/ADSR.cpp
        src/dsp/utils/HannWindow.cpp
        src/dsp/utils/Scales.cpp
        src/dsp/utils/SmoothedValue.cpp
        src/dsp/utils/Limiter.cpp
        src/dsp/utils/Random.cpp
        src/dsp/utils/DynamicDelayLine.cpp
        src/dsp/utils/DynamicAllpass.cpp
        src/dsp/utils/SineOscillator.cpp
        src/dsp/utils/BrownianNoise.cpp
        src/dsp/utils/FrequencyShifter.cpp
        src/dsp/utils/PitchShifter.cpp


        src/dsp/synth/SineProcessor.cpp
        src/dsp/granular/GrainProcessor.cpp

        src/dsp/fx/IntellijelWavefolder.cpp
        src/dsp/fx/ConstellationReverb.cpp
        src/dsp/fx/StereoFDN.cpp

        src/dsp/transport/InternalTransportClock.cpp

        src/dsp/metronome/MetronomePlayer.cpp
    )

    set_target_properties(${PROJECT_NAME}_dsp PROPERTIES 
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

# Resonator is part of DSP.
if(TANH_BUILD_DSP)
    add_library(${PROJECT_NAME}_resonator
        src/dsp/rings-resonator/RingsModalResonator.cpp
        src/dsp/rings-resonator/RingsString.cpp
        src/dsp/rings-resonator/RingsVoiceManager.cpp
        src/dsp/rings-resonator/RingsFmVoice.cpp
        src/dsp/rings-resonator/RingsStringSynthPart.cpp
        src/dsp/synth/RingsResonatorSynthProcessor.cpp
    )

    set_target_properties(${PROJECT_NAME}_resonator PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})

    add_library(${PROJECT_NAME}::Resonator ALIAS ${PROJECT_NAME}_resonator)
    # Compatibility alias: ResonatorCore now maps to the unified Resonator target.
    add_library(${PROJECT_NAME}::ResonatorCore ALIAS ${PROJECT_NAME}_resonator)

    target_include_directories(${PROJECT_NAME}_resonator
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    target_link_libraries(${PROJECT_NAME}_resonator
        PUBLIC
        ${PROJECT_NAME}_dsp
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${PROJECT_NAME}_resonator PRIVATE
            -Wno-unused-local-typedefs
            -Wno-unused-parameter
            -Wno-sign-compare
            -Wno-unused-variable
            -Wno-c++11-narrowing
        )
    endif()

    list(APPEND TANH_BUILT_COMPONENTS ${PROJECT_NAME}_resonator)
endif()

# Modulation component (depends on DSP and State)
if(TANH_BUILD_MODULATION)
    if(NOT TARGET ${PROJECT_NAME}_dsp)
        message(FATAL_ERROR "Modulation component requires DSP component")
    endif()
    if(NOT TARGET ${PROJECT_NAME}_state)
        message(FATAL_ERROR "Modulation component requires State component")
    endif()

    # moodycamel/readerwriterqueue — header-only lock-free SPSC queue used by
    # InputEventQueue to transport UI-thread events to the audio thread.
    FetchContent_Declare(
        readerwriterqueue
        GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
        GIT_TAG v1.0.6
        GIT_SHALLOW TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(readerwriterqueue)

    add_library(${PROJECT_NAME}_modulation
        src/modulation/ModulationMatrix.cpp
        src/modulation/LFOSource.cpp
        src/modulation/InputEventQueue.cpp
    )

    set_target_properties(${PROJECT_NAME}_modulation PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})

    add_library(${PROJECT_NAME}::Modulation ALIAS ${PROJECT_NAME}_modulation)

    # readerwriterqueue is used in a public header (InputEventQueue.h). Its
    # INTERFACE target carries a bare source-tree include path that cannot be
    # exported, so add the directory ourselves: the build tree reads it from
    # the FetchContent checkout, an installed package from the headers that
    # readerwriterqueue's own install rule places under include/readerwriterqueue.
    target_include_directories(${PROJECT_NAME}_modulation
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${readerwriterqueue_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/readerwriterqueue>
    )

    target_link_libraries(${PROJECT_NAME}_modulation
        PUBLIC
        ${PROJECT_NAME}_dsp
        ${PROJECT_NAME}_state
    )

    target_compile_definitions(${PROJECT_NAME}_modulation
        PUBLIC
        -DTANH_MODULATION_ENABLED
    )

    list(APPEND TANH_BUILT_COMPONENTS ${PROJECT_NAME}_modulation)
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
        SYSTEM
    )
    set(MINIAUDIO_NO_EXTRA_NODES ON CACHE BOOL "" FORCE)
    set(MINIAUDIO_NO_LIBVORBIS ON CACHE BOOL "" FORCE)
    set(MINIAUDIO_NO_LIBOPUS ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(miniaudio)

    # Suppress warnings from miniaudio source compilation
    target_compile_options(miniaudio PRIVATE -w)

    # On iOS, miniaudio.c includes AVFoundation which contains Objective-C
    # syntax. The FetchContent target compiles it as plain C, so we must
    # force Objective-C compilation and disable code signing.
    if(TANH_OPERATING_SYSTEM STREQUAL "iOS")
        # Force miniaudio.c to compile as Objective-C
        set_source_files_properties(
            ${miniaudio_SOURCE_DIR}/miniaudio.c
            TARGET_DIRECTORY miniaudio
            PROPERTIES COMPILE_FLAGS "-x objective-c"
        )

        get_property(miniaudio_targets DIRECTORY ${miniaudio_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
        tanh_ios_disable_code_signing(${miniaudio_targets})
    endif()


    add_library(${PROJECT_NAME}_audio_io
        src/audio-io/AudioDeviceManager.cpp
        src/audio-io/AudioFileLoader.cpp
        src/audio-io/AudioFileSink.cpp
        src/audio-io/AudioPlayerSource.cpp
        src/audio-io/DataSource.cpp
        src/audio-io/miniaudio_impl.cpp
    )

    if(TANH_OPERATING_SYSTEM STREQUAL "Android")
        target_sources(${PROJECT_NAME}_audio_io PRIVATE src/audio-io/AndroidAudioDevices.cpp)
    endif()

    if(TANH_OPERATING_SYSTEM STREQUAL "iOS")
        target_sources(${PROJECT_NAME}_audio_io PRIVATE src/audio-io/iOSAudioDevices.mm)
    endif()

    # On Apple platforms, miniaudio implementation includes Objective-C code
    # for CoreAudio/AVFoundation, so it must be compiled as Objective-C++
    if(APPLE)
        set_source_files_properties(
            src/audio-io/miniaudio_impl.cpp
            PROPERTIES COMPILE_FLAGS "-x objective-c++"
        )
    endif()

    set_target_properties(${PROJECT_NAME}_audio_io PROPERTIES 
        VERSION ${PROJECT_VERSION} 
        SOVERSION ${PROJECT_VERSION_MAJOR})

    add_library(${PROJECT_NAME}::AudioIO ALIAS ${PROJECT_NAME}_audio_io)

    # miniaudio is an implementation detail (no public header includes it);
    # expose it in the build tree only so the exported target stays relocatable.
    target_include_directories(${PROJECT_NAME}_audio_io
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${miniaudio_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    target_link_libraries(${PROJECT_NAME}_audio_io PUBLIC ${PROJECT_NAME}_core)

    # Link platform-specific frameworks required by miniaudio
    if(TANH_OPERATING_SYSTEM STREQUAL "iOS")
        target_link_libraries(${PROJECT_NAME}_audio_io PUBLIC
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreAudio"
            "-framework CoreMIDI"
            "-framework Accelerate"
            "-framework Foundation"
            "-lobjc"
        )
    elseif(TANH_OPERATING_SYSTEM STREQUAL "macOS")
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

# ==============================================================================
# Symbol policy and common settings of every component (cmake/tanh/symbol-policy.cmake)
# ==============================================================================
#
# Hidden visibility on every platform; TANH_API (include/tanh/core/Exports.h) is the
# allowlist — TANH_BUILDING (PRIVATE) selects dllexport while a component is compiled,
# TANH_STATIC (PUBLIC, so it reaches consumers through the CMake package) makes
# TANH_API empty for static components, whose objects become part of the consumer and
# must neither be exported from it nor be dllimport'ed. Shared components additionally
# pin their export table to namespace thl at link time (generated version script /
# -exported_symbols_list): -fvisibility=hidden cannot hide what a header stamps
# default-visibility itself (libstdc++ wraps namespace std in visibility("default")),
# and miniaudio, nlohmann::json and moodycamel must never leave the component that
# embeds them. test/exports verifies the result in CTest. Static components in a
# plugin are hidden by construction; the plugin's own export list does the rest.
include(cmake/tanh/symbol-policy.cmake)
include(cmake/tanh/sanitizers.cmake)

foreach(target ${TANH_BUILT_COMPONENTS})
    tanh_apply_symbol_policy(${target} EXPORT_PREFIX TANH)
    tanh_set_export_allowlist(${target} NAMESPACE thl)

    # Only apps need a signature on iOS.
    tanh_ios_disable_code_signing(${target})

    if(TANH_WITH_RTSAN)
        tanh_add_sanitizer(${target} rtsan DEFINE TANH_WITH_RTSAN)
    endif()
    if(TANH_WITH_ASAN)
        tanh_add_sanitizer(${target} asan DEFINE TANH_WITH_ASAN
            IGNORELIST "${CMAKE_CURRENT_SOURCE_DIR}/sanitizers/asan_ignorelist.txt")
    endif()
    if(TANH_WITH_USAN)
        tanh_add_sanitizer(${target} ubsan DEFINE TANH_WITH_USAN)
    endif()
    if(TANH_WITH_TSAN)
        tanh_add_sanitizer(${target} tsan DEFINE TANH_WITH_TSAN)
    endif()
    if(TANH_WITH_MSAN)
        tanh_add_sanitizer(${target} msan DEFINE TANH_WITH_MSAN)
    endif()
    if(TANH_WITH_LSAN)
        tanh_add_sanitizer(${target} lsan DEFINE TANH_WITH_LSAN)
    endif()
endforeach()

# ==============================================================================
# Add install targets for the library
# ==============================================================================

# Exported (installed) target names: tanh::Core, tanh::State, ... — the same
# names the build-tree ALIAS targets use, so `find_package(tanh COMPONENTS Core)`
# and `add_subdirectory(tanh-lib)` consumers link identically.
foreach(_pair
        "${PROJECT_NAME}_core=Core"
        "${PROJECT_NAME}_state=State"
        "${PROJECT_NAME}_dsp=DSP"
        "${PROJECT_NAME}_resonator=Resonator"
        "${PROJECT_NAME}_modulation=Modulation"
        "${PROJECT_NAME}_audio_io=AudioIO")
    string(REPLACE "=" ";" _pair "${_pair}")
    list(GET _pair 0 _target)
    list(GET _pair 1 _export_name)
    if(TARGET ${_target})
        set_target_properties(${_target} PROPERTIES EXPORT_NAME ${_export_name})
    endif()
endforeach()

if(TANH_WITH_INSTALL)
    include(cmake/install.cmake)
endif()
if(TANH_WITH_PACKAGING)
    if(NOT PROJECT_IS_TOP_LEVEL)
        message(FATAL_ERROR "TANH_WITH_PACKAGING is only supported when tanh-lib is the top-level project")
    endif()
    include(cmake/tanh/package.cmake)
    tanh_cpack_debian(
        VENDOR "tanh-lab"
        CONTACT "fares.schulz@tu-berlin.de"
        RUNTIME_DESCRIPTION "modular real-time-safe C++ audio library"
        DEV_DESCRIPTION "headers and CMake package for libtanh${PROJECT_VERSION_MAJOR}")
endif()

# ==============================================================================
# Add tests, examples, and documentation
# ==============================================================================

if (TANH_WITH_TESTS)
    # Suppress the CTest dashboard targets, then googletest / google benchmark once.
    set_property(GLOBAL PROPERTY CTEST_TARGETS_ADDED 1)
    include(CTest)
    include(cmake/tanh/test-deps.cmake)
    tanh_fetch_googletest(VERSION v1.14.0)
    tanh_fetch_googlebenchmark(VERSION v1.9.1)
    if (TANH_BUILD_CORE)
        add_subdirectory(test/core)
    endif()
    if (TANH_BUILD_STATE)
        add_subdirectory(test/state)
    endif()
    if (TANH_BUILD_DSP)
        add_subdirectory(test/dsp)
    endif()
    if (TANH_BUILD_MODULATION)
        add_subdirectory(test/modulation)
    endif()
    if (TANH_BUILD_AUDIO_IO)
        add_subdirectory(test/audio-io)
    endif()

    # The export-table invariant, on the desktop formats where it can be inspected.
    if(NOT CMAKE_CROSSCOMPILING AND TANH_BINARY_FORMAT MATCHES "^(ELF|Mach-O|PE)$")
        add_subdirectory(test/exports)
    endif()

    # On iOS, test executables become .app bundles and need a bundle identifier
    # for simctl install/launch to work (no-op elsewhere).
    set(_ios_team "")
    if(DEVELOPMENT_TEAM)
        set(_ios_team DEVELOPMENT_TEAM "${DEVELOPMENT_TEAM}")
    endif()
    foreach(_test test_core test_state test_dsp test_modulation test_audio_io benchmark_state benchmark_modulation)
        if(TARGET ${_test})
            tanh_ios_test_bundle(${_test} BUNDLE_ID_PREFIX com.tanh ${_ios_team})
        endif()
    endforeach()
endif()

if(TANH_WITH_EXAMPLES AND TANH_OPERATING_SYSTEM STREQUAL "iOS")
    add_subdirectory(examples/ios/audio-io)
endif()


# if (TANH_WITH_DOCS)
#     add_subdirectory(docs)
# endif()
