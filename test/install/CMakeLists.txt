# Consumer of an installed tanh package. Not part of the main build: CI (and
# `just test-install`) builds it against a fresh `cmake --install` prefix to
# verify that the install tree carries everything find_package(tanh) needs.
cmake_minimum_required(VERSION 3.25)
project(tanh_install_consumer CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(tanh REQUIRED COMPONENTS Core)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE tanh::Core)
