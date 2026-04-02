# Default recipe - show available commands
default:
    @just --list

# Configure desktop debug build
configure:
    cmake --preset desktop-debug

# Build desktop debug
build: configure
    cmake --build --preset desktop-debug --parallel

# Run all tests
test: build
    ctest --preset desktop-debug

# Run tests with verbose output
test-verbose: build
    ctest --preset desktop-debug --verbose

# Run only audio_io tests
test-audio: build
    ./build-desktop/test/audio-io/test_audio_io

# Run hardware tests (requires audio devices)
test-hardware: build
    ./build-desktop/test/audio-io/test_audio_io --gtest_also_run_disabled_tests --gtest_filter="HardwareTests.*"

# Run a specific test by name pattern
test-filter PATTERN: build
    ctest --preset desktop-debug -R "{{ PATTERN }}"

# Clean build artefacts
clean:
    rm -rf build-desktop build-ios-simulator build-ios-device

# Rebuild from scratch
rebuild: clean build

# Run clang-format on source files
format:
    find src include examples -name "*.cpp" -o -name "*.h" -o -name "*.mm" | xargs clang-format -i

# Check formatting without modifying files
format-check:
    find src include examples -name "*.cpp" -o -name "*.h" -o -name "*.mm" | xargs clang-format --dry-run --Werror

# Run clang-tidy on source files (check only)
tidy:
    find src -name "*.cpp" -o -name "*.mm" | xargs clang-tidy -p build-desktop/

# Run clang-tidy and auto-fix where possible
tidy-fix:
    find src -name "*.cpp" -o -name "*.mm" | xargs clang-tidy -p build-desktop/ --fix

# Build desktop release
build-release:
    cmake --preset desktop-release
    cmake --build --preset desktop-release --parallel

# Configure for iOS Simulator
configure-ios-sim:
    cmake --preset ios-simulator-debug

# Configure for iOS Device
configure-ios-device team=`./scripts/resolve-team-id.sh`:
    cmake --preset ios-device-debug -DDEVELOPMENT_TEAM={{ team }}

# Build iOS example for simulator
build-ios-sim: configure-ios-sim
    cmake --build --preset ios-simulator-debug --parallel

# Build iOS example for device
build-ios-device team=`./scripts/resolve-team-id.sh`: (configure-ios-device team)
    cmake --build --preset ios-device-debug --parallel

# Open iOS project in Xcode (configured for simulator)
open-xcode-ios-sim: configure-ios-sim
    @echo "Opening Xcode project. Select 'audio_io' scheme to build the example app."
    open build-ios-simulator/tanh.xcodeproj

# Open iOS project in Xcode (configured for device)
open-xcode-ios-device team=`./scripts/resolve-team-id.sh`: (configure-ios-device team)
    @echo "Opening Xcode project. Select 'audio_io' scheme."
    open build-ios-device/tanh.xcodeproj
