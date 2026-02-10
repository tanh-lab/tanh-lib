# Default recipe - show available commands
default:
    @just --list

# Configure the project
configure:
    cmake -B build -S . -DTANH_WITH_TESTS=ON

# Build the project
build: configure
    cmake --build build --parallel

# Run all tests
test: build
    ctest --test-dir build --output-on-failure

# Run tests with verbose output
test-verbose: build
    ctest --test-dir build --output-on-failure --verbose

# Run only audio_io tests
test-audio: build
    ./build/test/audio_io/test_audio_io

# Run hardware tests (requires audio devices)
test-hardware: build
    ./build/test/audio_io/test_audio_io --gtest_also_run_disabled_tests --gtest_filter="HardwareTests.*"

# Run a specific test by name pattern
test-filter PATTERN: build
    ctest --test-dir build --output-on-failure -R "{{ PATTERN }}"

# Clean build artefacts
clean:
    rm -rf build

# Rebuild from scratch
rebuild: clean build

# Run clang-format on source files
format:
    fd -e cpp -e h -E build . | xargs clang-format -i

# Check formatting without modifying files
format-check:
    fd -e cpp -e h -E build . | xargs clang-format --dry-run --Werror

# Generate compile_commands.json for IDE support
compile-commands:
    cmake -B build -S . -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    ln -sf build/compile_commands.json .

# Build with debug symbols
build-debug:
    cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DTANH_WITH_TESTS=ON
    cmake --build build --parallel

# Build optimised release
build-release:
    cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DTANH_WITH_TESTS=ON
    cmake --build build --parallel

# Configure for iOS Simulator
configure-ios-sim:
    cmake -B build-ios -S . \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_SYSROOT=iphonesimulator \
        -DTANH_WITH_TESTS=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        -G Xcode

# Configure for iOS Device (physical iPhone/iPad)
# Optional: pass TEAM_ID for automatic signing (requires Apple ID logged into Xcode first)
configure-ios-device TEAM_ID="":
    cmake -B build-ios -S . \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_SYSROOT=iphoneos \
        -DTANH_WITH_TESTS=OFF \
        -DBUILD_SHARED_LIBS=OFF \
        {{ if TEAM_ID != "" { "-DDEVELOPMENT_TEAM=" + TEAM_ID } else { "" } }} \
        -G Xcode

# Configure for iOS (defaults to device)
configure-ios TEAM_ID="": (configure-ios-device TEAM_ID)

# Build iOS example
build-ios TEAM_ID="": (configure-ios TEAM_ID)
    cmake --build build-ios --target audio_io --parallel

# Build iOS example for simulator
build-ios-sim: configure-ios-sim
    cmake --build build-ios --target audio_io --parallel

# Open iOS project in Xcode (configured for device)
# Pass TEAM_ID for automatic signing, or select team manually in Xcode
open-ios TEAM_ID="": (configure-ios TEAM_ID)
    @echo "Opening Xcode project. Select 'audio_io' scheme."
    open build-ios/tanh.xcodeproj

# Open iOS project in Xcode (configured for simulator)
open-ios-sim: configure-ios-sim
    @echo "Opening Xcode project. Select 'audio_io' scheme to build the example app."
    open build-ios/tanh.xcodeproj
