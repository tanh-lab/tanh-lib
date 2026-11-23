# Consumer of an installed tanh package built without State: a host that brings
# its own parameter store (ParameterBackend). Not part of the main build: CI (and
# `just test-install-modulation`) builds it against a fresh `cmake --install`
# prefix to verify that Modulation resolves DSP and Parameters, and not State.
cmake_minimum_required(VERSION 3.25)
project(tanh_install_modulation_consumer CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(tanh REQUIRED COMPONENTS Modulation)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE tanh::Modulation)
