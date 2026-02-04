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
