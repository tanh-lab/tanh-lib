# ==============================================================================
# cmake/fft-backend.cmake — the FFT behind thl::dsp::fft::RealFft.
#
#   TANH_FFT_BACKEND   pffft (default, every platform) or accelerate (macOS, iOS)
#   tanh_add_fft_backend(<target>)   adds the chosen backend to <target> (the DSP component):
#                                    its RealFft source and, for pffft, pffft's float FFT
#                                    compiled in as a private object library; for
#                                    accelerate, the Accelerate framework.
#
# One backend per build; RealFft gives the same bins on either (test/dsp/test_RealFft.cpp).
# pffft is fetched as sources at a pinned tag (its own CMake project, tests and benchmarks
# are not added). Its licence is BSD-like (FFTPACK): binary distributions must carry its
# notice. Its C symbols (pffft_*) stay inside the component: hidden here, and the shared
# component's export list allows namespace thl only (test/exports, test/abi-namespace).
# ==============================================================================
include_guard(GLOBAL)
include(FetchContent)

set(TANH_FFT_BACKEND "pffft" CACHE STRING "FFT backend of tanh::DSP: pffft or accelerate")
set_property(CACHE TANH_FFT_BACKEND PROPERTY STRINGS pffft accelerate)

function(tanh_add_fft_backend target)
    if(TANH_FFT_BACKEND STREQUAL "accelerate")
        if(NOT (TANH_OPERATING_SYSTEM STREQUAL "macOS" OR TANH_OPERATING_SYSTEM STREQUAL "iOS"))
            message(FATAL_ERROR
                "TANH_FFT_BACKEND=accelerate needs macOS or iOS (this is ${TANH_OPERATING_SYSTEM})")
        endif()
        target_sources(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src/dsp/fft/RealFftAccelerate.cpp)
        target_link_libraries(${target} PRIVATE "-framework Accelerate")
    elseif(TANH_FFT_BACKEND STREQUAL "pffft")
        # Sources only (SOURCE_SUBDIR keeps its own CMake project, tests and benchmarks out):
        # its float FFT is compiled into the DSP component as a private object library.
        FetchContent_Declare(
            pffft
            GIT_REPOSITORY https://github.com/marton78/pffft.git
            GIT_TAG v1.1.0
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR do-not-add
            SYSTEM
        )
        FetchContent_MakeAvailable(pffft)
        add_library(${target}_pffft OBJECT
            ${pffft_SOURCE_DIR}/src/pffft.c
            ${pffft_SOURCE_DIR}/src/pffft_common.c
        )
        target_include_directories(${target}_pffft SYSTEM PRIVATE
            ${pffft_SOURCE_DIR}/include/pffft
            ${pffft_SOURCE_DIR}/src
        )
        set_target_properties(${target}_pffft PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            C_VISIBILITY_PRESET hidden
        )
        # SSE comes with x86-64; NEON (and WebAssembly's SIMD128 through it) must be asked for,
        # or pffft falls back to scalar code. PFFFT_STATIC_DEFINE: compiled in, so no
        # dllimport/dllexport on Windows.
        target_compile_definitions(${target}_pffft
            PRIVATE _USE_MATH_DEFINES PFFFT_ENABLE_NEON PFFFT_STATIC_DEFINE)
        # Its "SIMD macros are defined" notes are #pragma messages, not problems.
        target_compile_options(${target}_pffft PRIVATE
            $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-\#pragma-messages>)
        target_sources(${target} PRIVATE
            ${PROJECT_SOURCE_DIR}/src/dsp/fft/RealFftPffft.cpp
            $<TARGET_OBJECTS:${target}_pffft>
        )
        target_include_directories(${target} SYSTEM PRIVATE
            $<BUILD_INTERFACE:${pffft_SOURCE_DIR}/include/pffft>
        )
        set_source_files_properties(${PROJECT_SOURCE_DIR}/src/dsp/fft/RealFftPffft.cpp PROPERTIES
            COMPILE_DEFINITIONS PFFFT_STATIC_DEFINE)
    else()
        message(FATAL_ERROR "TANH_FFT_BACKEND must be pffft or accelerate, not ${TANH_FFT_BACKEND}")
    endif()
endfunction()
