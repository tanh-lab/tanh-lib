# iOS Toolchain for CMake
# Usage: cmake -B build -G Xcode -DCMAKE_TOOLCHAIN_FILE=${workspaceFolder}/cmake/toolchain/ios.toolchain.cmake -DTHL_IOS_PLATFORM=SIMULATOR

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_VERSION 15.0)
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0 CACHE STRING "Minimum iOS deployment version")

# Platform can be: DEVICE, SIMULATOR
# Use environment variable to persist across compiler check phases
if(NOT DEFINED THL_IOS_PLATFORM)
    # Check environment variable first
    if(DEFINED ENV{THL_IOS_PLATFORM})
        set(THL_IOS_PLATFORM "$ENV{THL_IOS_PLATFORM}")
    else()
        set(THL_IOS_PLATFORM "SIMULATOR")
    endif()
endif()

# Store in environment for subsequent invocations (compiler checks)
set(ENV{THL_IOS_PLATFORM} "${THL_IOS_PLATFORM}")

# Also store in cache
set(THL_IOS_PLATFORM "${THL_IOS_PLATFORM}" CACHE STRING "iOS Platform: DEVICE or SIMULATOR" FORCE)

message(STATUS "THL_IOS_PLATFORM ${THL_IOS_PLATFORM}")

if(THL_IOS_PLATFORM STREQUAL "DEVICE")
    # iOS Device (arm64)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
    set(CMAKE_OSX_SYSROOT iphoneos)
    set(CMAKE_XCODE_EFFECTIVE_PLATFORMS "-iphoneos")
    set(IOS_PLATFORM_LOCATION "iPhoneOS.platform")

elseif(THL_IOS_PLATFORM STREQUAL "SIMULATOR")
    # iOS Simulator (arm64 for Apple Silicon Macs)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
    set(CMAKE_OSX_SYSROOT iphonesimulator)
    set(CMAKE_XCODE_EFFECTIVE_PLATFORMS "-iphonesimulator")
    set(IOS_PLATFORM_LOCATION "iPhoneSimulator.platform")

else()
    message(FATAL_ERROR "Unknown THL_IOS_PLATFORM: ${THL_IOS_PLATFORM}. Use DEVICE or SIMULATOR") 
endif()

# Find the SDK path
execute_process(
    COMMAND xcrun --sdk ${CMAKE_OSX_SYSROOT} --show-sdk-path
    OUTPUT_VARIABLE SDK_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
set(CMAKE_OSX_SYSROOT ${SDK_PATH})

# Enable ARC and modern Objective-C features
set(CMAKE_XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
set(CMAKE_XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES YES)

# Bitcode is deprecated
set(CMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE NO)

# Lock architectures — ONLY_ACTIVE_ARCH only works in Xcode IDE
# (requires a run destination). For CLI builds, explicitly set ARCHS
# so Xcode doesn't attempt to build for all default architectures.
set(CMAKE_XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO)
set(CMAKE_XCODE_ATTRIBUTE_ARCHS "${CMAKE_OSX_ARCHITECTURES}")

# Visibility
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)

# CTest: use wrapper scripts so ctest can run iOS test bundles
if(THL_IOS_PLATFORM STREQUAL "SIMULATOR")
    set(CMAKE_CROSSCOMPILING_EMULATOR
        "${CMAKE_CURRENT_LIST_DIR}/../../scripts/run-ios-sim.sh"
        CACHE STRING "iOS simulator test runner" FORCE)
elseif(THL_IOS_PLATFORM STREQUAL "DEVICE")
    set(CMAKE_CROSSCOMPILING_EMULATOR
        "${CMAKE_CURRENT_LIST_DIR}/../../scripts/run-ios-device.sh"
        CACHE STRING "iOS device test runner" FORCE)
endif()

message(STATUS "iOS Toolchain Configuration:")
message(STATUS "  Platform: ${THL_IOS_PLATFORM}")
message(STATUS "  Architecture: ${CMAKE_OSX_ARCHITECTURES}")
message(STATUS "  SDK: ${CMAKE_OSX_SYSROOT}")
message(STATUS "  Deployment Target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")