# The native half of the Android MIDI test app: tanh::Midi, built for Android, behind a few
# JNI functions (see ../run.sh).
cmake_minimum_required(VERSION 3.25)
project(miditest LANGUAGES C CXX)

set(BUILD_SHARED_LIBS OFF)
set(TANH_WITH_TESTS OFF)
set(TANH_WITH_EXAMPLES OFF)
set(TANH_WITH_INSTALL OFF)
set(TANH_BUILD_STATE OFF)
set(TANH_BUILD_DSP OFF)
set(TANH_BUILD_MODULATION OFF)
set(TANH_BUILD_AUDIO_IO OFF)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../.. tanh-lib)

add_library(miditest SHARED midi_test_jni.cpp)
target_link_libraries(miditest PRIVATE tanh::Midi)
