#include <gtest/gtest.h>
#include "tanh/audio-io/AudioDeviceInfo.h"
#include "tanh/audio-io/AudioDeviceManager.h"
#include "tanh/audio-io/AudioFileLoader.h"
#include "tanh/audio-io/AudioFileSink.h"
#include "tanh/audio-io/AudioIODeviceCallback.h"
#include "tanh/audio-io/AudioPlayerSource.h"
#include "tanh/audio-io/DataSource.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <tanh/core/Numbers.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace thl;

// =============================================================================
// AudioDeviceInfo Tests
// =============================================================================

TEST(AudioDeviceInfo, DefaultConstruction) {
    AudioDeviceInfo info;
    EXPECT_TRUE(info.m_name.empty());
    EXPECT_TRUE(info.m_sample_rates.empty());
}

TEST(AudioDeviceInfo, DeviceIDPointer) {
    AudioDeviceInfo info;
    const void* ptr = info.device_id_ptr();
    EXPECT_NE(ptr, nullptr);
}

// =============================================================================
// AudioDeviceManager Tests
// =============================================================================

TEST(AudioDeviceManager, ConstructionAndDestruction) {
    // Should not crash or leak
    AudioDeviceManager manager;
    // Context should be initialised on most systems
    // (may fail on headless CI, so we just check it doesn't crash)
}

TEST(AudioDeviceManager, ContextInitialisation) {
    AudioDeviceManager manager;
    // On systems with audio, this should be true
    // On headless CI, it might be false - either way, shouldn't crash
    bool initialised = manager.is_context_initialised();
    (void)initialised;  // Suppress unused warning
}

TEST(AudioDeviceManager, DefaultValues) {
    AudioDeviceManager manager;
    EXPECT_EQ(manager.get_sample_rate(), 44100);
    EXPECT_EQ(manager.get_buffer_size(), 0);
    EXPECT_EQ(manager.get_num_output_channels(), 1);
    EXPECT_EQ(manager.get_num_input_channels(), 1);
    EXPECT_FALSE(manager.is_playback_running());
}

TEST(AudioDeviceManager, EnumerateDevicesDoesNotCrash) {
    AudioDeviceManager manager;

    // These should not crash even if no devices are available
    auto input_devices = manager.enumerate_input_devices();
    auto output_devices = manager.enumerate_output_devices();

    // We can't assert specific device counts as this depends on the system
    // Just verify the calls complete without crashing
}

TEST(AudioDeviceManager, ShutdownWithoutInitialise) {
    AudioDeviceManager manager;
    // Calling shutdown without initialise should be safe
    manager.shutdown();
    manager.shutdown();  // Multiple calls should be safe
}

TEST(AudioDeviceManager, StopWithoutStart) {
    AudioDeviceManager manager;
    // Calling stop without start should be safe
    manager.stop_playback();
}

// =============================================================================
// AudioIODeviceCallback Tests
// =============================================================================

class MockCallback : public AudioIODeviceCallback {
public:
    int m_prepare_call_count = 0;
    int m_release_call_count = 0;
    int m_process_call_count = 0;

    void process(float* output_buffer,
                 const float* input_buffer,
                 uint32_t frame_count,
                 uint32_t /*numInputChannels*/,
                 uint32_t num_output_channels) override {
        ++m_process_call_count;
        // Fill output with silence
        if (!output_buffer || num_output_channels == 0) { return; }
        for (uint32_t i = 0; i < frame_count * num_output_channels; ++i) {
            output_buffer[i] = 0.0f;
        }
        (void)input_buffer;
    }

    void prepare_to_play(uint32_t sample_rate, uint32_t buffer_size) override {
        ++m_prepare_call_count;
        (void)sample_rate;
        (void)buffer_size;
    }

    void release_resources() override { ++m_release_call_count; }
};

TEST(AudioIODeviceCallback, MockCallbackConstruction) {
    MockCallback callback;
    EXPECT_EQ(callback.m_prepare_call_count, 0);
    EXPECT_EQ(callback.m_release_call_count, 0);
    EXPECT_EQ(callback.m_process_call_count, 0);
}

TEST(AudioIODeviceCallback, ProcessFillsSilence) {
    MockCallback callback;

    constexpr size_t k_frame_count = 256;
    constexpr size_t k_num_channels = 2;
    std::array<float, k_frame_count * k_num_channels> output{};
    std::array<float, k_frame_count * k_num_channels> input{};

    // Fill output with non-zero values
    for (auto& sample : output) { sample = 1.0f; }

    callback.process(output.data(), input.data(), k_frame_count, k_num_channels, k_num_channels);

    EXPECT_EQ(callback.m_process_call_count, 1);

    // Verify output is now silence
    for (const auto& sample : output) { EXPECT_FLOAT_EQ(sample, 0.0f); }
}

TEST(AudioDeviceManager, AddRemoveCallback) {
    AudioDeviceManager manager;
    MockCallback callback;

    // Adding and removing callbacks should not crash
    manager.add_playback_callback(&callback);
    manager.remove_playback_callback(&callback);

    // Removing non-existent callback should be safe
    manager.remove_playback_callback(&callback);
}

TEST(AudioDeviceManager, MultipleCallbacks) {
    AudioDeviceManager manager;
    MockCallback callback1;
    MockCallback callback2;
    MockCallback callback3;

    manager.add_playback_callback(&callback1);
    manager.add_playback_callback(&callback2);
    manager.add_playback_callback(&callback3);

    manager.remove_playback_callback(&callback2);
    manager.remove_playback_callback(&callback1);
    manager.remove_playback_callback(&callback3);
}

TEST(AudioDeviceManager, ConcurrentCallbackModification) {
    AudioDeviceManager manager;

    constexpr int k_num_callbacks = 8;
    std::array<MockCallback, k_num_callbacks> callbacks{};

    std::atomic<bool> running{true};
    std::atomic<int> add_count{0};
    std::atomic<int> remove_count{0};

    // Multiple threads adding/removing callbacks concurrently
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            while (running.load(std::memory_order_relaxed)) {
                int idx = (t * 2) % k_num_callbacks;
                manager.add_playback_callback(&callbacks[static_cast<size_t>(idx)]);
                add_count.fetch_add(1, std::memory_order_relaxed);

                std::this_thread::sleep_for(std::chrono::microseconds(50));

                manager.remove_playback_callback(&callbacks[static_cast<size_t>(idx)]);
                remove_count.fetch_add(1, std::memory_order_relaxed);

                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }

    // Let threads run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running.store(false, std::memory_order_relaxed);

    for (auto& thread : threads) { thread.join(); }

    // Verify operations completed without crashing
    EXPECT_GT(add_count.load(), 0);
    EXPECT_GT(remove_count.load(), 0);
}

// =============================================================================
// Bluetooth Buffer Size Clamping Tests
// =============================================================================

TEST(AudioDeviceManager, BluetoothClampNoOpWhenBelowLimit) {
    // 512 frames @ 16 kHz = 32 ms, well below the 64 ms cap.
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(512, 16000), 512u);
}

TEST(AudioDeviceManager, BluetoothClampAtExactLimit) {
    // 1024 frames @ 16 kHz = exactly 64 ms — should NOT be clamped.
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(1024, 16000), 1024u);
}

TEST(AudioDeviceManager, BluetoothClampAboveLimit) {
    // 2048 frames @ 16 kHz = 128 ms — must be clamped to 1024.
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(2048, 16000), 1024u);
}

TEST(AudioDeviceManager, BluetoothClampAt48kHz) {
    // At 48 kHz the 64 ms cap is 3072 frames.
    auto max_frames = static_cast<uint32_t>(48000 * 0.064f);
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(4096, 48000), max_frames);
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(3072, 48000), 3072u);
}

TEST(AudioDeviceManager, BluetoothClampAt44100Hz) {
    auto max_frames = static_cast<uint32_t>(44100 * 0.064f);
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(4096, 44100), max_frames);
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(256, 44100), 256u);
}

TEST(AudioDeviceManager, BluetoothClampZeroSampleRate) {
    // Zero sample rate should return the original size (no division by zero).
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(2048, 0), 2048u);
}

TEST(AudioDeviceManager, BluetoothClampSmallBuffer) {
    // Very small buffer should pass through unchanged.
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(64, 16000), 64u);
}

// =============================================================================
// Hardware Tests (DISABLED by default - run with
// --gtest_also_run_disabled_tests)
// =============================================================================

class SineCallback : public AudioIODeviceCallback {
public:
    std::atomic<int> m_process_call_count{0};
    double m_frequency = 440.0;
    double m_phase = 0.0;
    double m_sample_rate = 48000.0;

    explicit SineCallback(double freq = 440.0) : m_frequency(freq) {}

    void process(float* output_buffer,
                 const float* /*inputBuffer*/,
                 uint32_t frame_count,
                 uint32_t /*numInputChannels*/,
                 uint32_t num_output_channels) override {
        ++m_process_call_count;
        if (!output_buffer || num_output_channels == 0) { return; }

        double phase_increment = (2.0 * std::numbers::pi * m_frequency) / m_sample_rate;

        for (uint32_t frame = 0; frame < frame_count; ++frame) {
            auto sample = static_cast<float>(std::sin(m_phase) * 0.3);
            m_phase += phase_increment;
            if (m_phase >= 2.0 * std::numbers::pi) { m_phase -= 2.0 * std::numbers::pi; }

            for (uint32_t ch = 0; ch < num_output_channels; ++ch) {
                output_buffer[frame * num_output_channels + ch] = sample;
            }
        }
    }

    void prepare_to_play(uint32_t rate, uint32_t /*bufferSize*/) override {
        m_sample_rate = static_cast<double>(rate);
        m_phase = 0.0;
    }
};

TEST(HardwareTests, DISABLED_DeviceEnumeration) {
    AudioDeviceManager manager;

    if (!manager.is_context_initialised()) { GTEST_SKIP() << "Audio context not available"; }

    auto inputs = manager.enumerate_input_devices();
    auto outputs = manager.enumerate_output_devices();

    std::cout << "\n=== Input Devices (" << inputs.size() << ") ===\n";
    for (const auto& device : inputs) {
        std::cout << "  - " << device.m_name << " (rates: ";
        for (size_t i = 0; i < device.m_sample_rates.size(); ++i) {
            if (i > 0) { std::cout << ", "; }
            std::cout << device.m_sample_rates[i];
        }
        std::cout << ")\n";
    }

    std::cout << "\n=== Output Devices (" << outputs.size() << ") ===\n";
    for (const auto& device : outputs) {
        std::cout << "  - " << device.m_name << " (rates: ";
        for (size_t i = 0; i < device.m_sample_rates.size(); ++i) {
            if (i > 0) { std::cout << ", "; }
            std::cout << device.m_sample_rates[i];
        }
        std::cout << ")\n";
    }

    EXPECT_FALSE(outputs.empty()) << "No output devices found";
}

TEST(HardwareTests, DISABLED_PlaySineWave) {
    AudioDeviceManager manager;

    if (!manager.is_context_initialised()) { GTEST_SKIP() << "Audio context not available"; }

    auto outputs = manager.enumerate_output_devices();
    if (outputs.empty()) { GTEST_SKIP() << "No output devices available"; }

    std::cout << "\nUsing output device: " << outputs[0].m_name << "\n";

    SineCallback sine_callback(440.0);

    bool initialised = manager.initialise(nullptr, &outputs[0], 48000, 256, 0, 2);
    ASSERT_TRUE(initialised) << "Failed to initialise audio device";

    manager.add_playback_callback(&sine_callback);

    ASSERT_TRUE(manager.start_playback()) << "Failed to start audio";
    EXPECT_TRUE(manager.is_playback_running());

    std::cout << "Playing 440Hz sine wave for 500ms...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    manager.stop_playback();
    EXPECT_FALSE(manager.is_playback_running());

    std::cout << "Process callback was called " << sine_callback.m_process_call_count << " times\n";

    EXPECT_GT(sine_callback.m_process_call_count.load(), 0) << "Process callback was never called";
}

TEST(HardwareTests, DISABLED_StartStopCycles) {
    AudioDeviceManager manager;

    if (!manager.is_context_initialised()) { GTEST_SKIP() << "Audio context not available"; }

    auto outputs = manager.enumerate_output_devices();
    if (outputs.empty()) { GTEST_SKIP() << "No output devices available"; }

    SineCallback callback;

    ASSERT_TRUE(manager.initialise(nullptr, &outputs[0], 48000, 512, 0, 2));
    manager.add_playback_callback(&callback);

    for (int i = 0; i < 3; ++i) {
        std::cout << "Start/stop cycle " << (i + 1) << "/3\n";

        ASSERT_TRUE(manager.start_playback());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        manager.stop_playback();

        EXPECT_GT(callback.m_process_call_count.load(), 0);
    }

    std::cout << "Total process calls: " << callback.m_process_call_count << "\n";
}

TEST(HardwareTests, DISABLED_FullDuplex) {
    AudioDeviceManager manager;

    if (!manager.is_context_initialised()) { GTEST_SKIP() << "Audio context not available"; }

    auto inputs = manager.enumerate_input_devices();
    auto outputs = manager.enumerate_output_devices();

    if (inputs.empty() || outputs.empty()) {
        GTEST_SKIP() << "Need both input and output devices for duplex test";
    }

    std::cout << "\nInput: " << inputs[0].m_name << "\n";
    std::cout << "Output: " << outputs[0].m_name << "\n";

    MockCallback callback;

    bool initialised = manager.initialise(&inputs[0], &outputs[0], 48000, 512, 2, 2);
    ASSERT_TRUE(initialised) << "Failed to initialise duplex device";

    manager.add_playback_callback(&callback);

    ASSERT_TRUE(manager.start_playback());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    manager.stop_playback();

    std::cout << "Duplex process calls: " << callback.m_process_call_count << "\n";
    EXPECT_GT(callback.m_process_call_count, 0);
}

// =============================================================================
// AudioFileSink Tests
// =============================================================================

TEST(AudioFileSink, DefaultConstruction) {
    AudioFileSink sink;
    EXPECT_FALSE(sink.is_open());
    EXPECT_FALSE(sink.is_recording());
    EXPECT_EQ(sink.get_frames_written(), 0);
}

TEST(AudioFileSink, CloseWithoutOpen) {
    AudioFileSink sink;
    sink.close_file();
    EXPECT_FALSE(sink.is_open());
}

TEST(AudioFileSink, StartRecordingWithoutOpen) {
    AudioFileSink sink;
    sink.start_recording();
    EXPECT_FALSE(sink.is_recording());
}

TEST(AudioFileSink, StopRecordingWithoutStart) {
    AudioFileSink sink;
    sink.stop_recording();
    EXPECT_FALSE(sink.is_recording());
}

TEST(AudioFileSink, ProcessWithoutOpen) {
    AudioFileSink sink;

    constexpr size_t k_frame_count = 256;
    constexpr size_t k_num_channels = 2;
    std::array<float, k_frame_count * k_num_channels> output{};
    std::array<float, k_frame_count * k_num_channels> input{};

    sink.process(output.data(), input.data(), k_frame_count, k_num_channels, 0);
    EXPECT_EQ(sink.get_frames_written(), 0);
}

TEST(AudioFileSink, OpenAndCloseFile) {
    AudioFileSink sink;

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_audio_sink.wav";

    bool opened = sink.open_file(test_file.string(), 2, 48000);
    EXPECT_TRUE(opened);
    EXPECT_TRUE(sink.is_open());
    EXPECT_FALSE(sink.is_recording());

    sink.close_file();
    EXPECT_FALSE(sink.is_open());

    std::filesystem::remove(test_file);
}

TEST(AudioFileSink, RecordingStateToggle) {
    AudioFileSink sink;

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_audio_sink_toggle.wav";

    ASSERT_TRUE(sink.open_file(test_file.string(), 2, 48000));

    EXPECT_FALSE(sink.is_recording());
    sink.start_recording();
    EXPECT_TRUE(sink.is_recording());
    sink.stop_recording();
    EXPECT_FALSE(sink.is_recording());

    sink.close_file();
    std::filesystem::remove(test_file);
}

TEST(AudioFileSink, WriteFrames) {
    AudioFileSink sink;

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_audio_sink_write.wav";

    ASSERT_TRUE(sink.open_file(test_file.string(), 2, 48000));
    sink.start_recording();

    constexpr size_t k_frame_count = 256;
    constexpr size_t k_num_channels = 2;
    std::array<float, k_frame_count * k_num_channels> output{};
    std::array<float, k_frame_count * k_num_channels> input{};

    for (size_t i = 0; i < k_frame_count * k_num_channels; ++i) {
        input[i] = static_cast<float>(i) / static_cast<float>(k_frame_count * k_num_channels);
    }

    sink.process(output.data(), input.data(), k_frame_count, k_num_channels, 0);
    EXPECT_EQ(sink.get_frames_written(), k_frame_count);

    sink.process(output.data(), input.data(), k_frame_count, k_num_channels, 0);
    EXPECT_EQ(sink.get_frames_written(), k_frame_count * 2);

    sink.close_file();
    EXPECT_TRUE(std::filesystem::exists(test_file));
    std::filesystem::remove(test_file);
}

TEST(AudioFileSink, RejectMismatchedInputChannels) {
    AudioFileSink sink;

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_audio_sink_channel_mismatch.wav";

    ASSERT_TRUE(sink.open_file(test_file.string(), 2, 48000));
    sink.start_recording();

    constexpr size_t k_frame_count = 256;
    constexpr size_t k_input_channels = 1;
    std::array<float, k_frame_count * 2> output{};
    std::array<float, k_frame_count * k_input_channels> input{};

    sink.process(output.data(), input.data(), k_frame_count, k_input_channels, 0);
    EXPECT_EQ(sink.get_frames_written(), 0);

    sink.close_file();
    std::filesystem::remove(test_file);
}

TEST(AudioFileSink, ReleaseResources) {
    AudioFileSink sink;

    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_audio_sink_release.wav";

    ASSERT_TRUE(sink.open_file(test_file.string(), 2, 48000));
    sink.start_recording();

    sink.release_resources();
    EXPECT_FALSE(sink.is_open());
    EXPECT_FALSE(sink.is_recording());

    std::filesystem::remove(test_file);
}

// =============================================================================
// AudioPlayerSource Tests
// =============================================================================

// =============================================================================
// AudioFileLoader Tests
// =============================================================================

// Disabled: intermittently aborts during teardown in the underlying backend
// after the assertion passes on this platform.
TEST(AudioFileLoader, LoadFromFileNonExistent) {
    thl::audio_io::AudioFileLoader loader;
    auto buffer = loader.load_from_file("/nonexistent/path/audio.wav", 48000, 2);
    EXPECT_TRUE(buffer.empty());
}

TEST(AudioFileLoader, LoadFromFileEmptyPath) {
    thl::audio_io::AudioFileLoader loader;
    auto buffer = loader.load_from_file("", 48000, 2);
    EXPECT_TRUE(buffer.empty());
}

TEST(AudioFileLoader, LoadFromMemoryNullData) {
    thl::audio_io::AudioFileLoader loader;
    auto buffer = loader.load_from_memory(nullptr, 0, 48000, 2);
    EXPECT_TRUE(buffer.empty());
}

TEST(AudioFileLoader, LoadFromMemoryZeroSize) {
    thl::audio_io::AudioFileLoader loader;
    std::array<uint8_t, 1> data{0};
    auto buffer = loader.load_from_memory(data.data(), 0, 48000, 2);
    EXPECT_TRUE(buffer.empty());
}

TEST(AudioFileLoader, LoadDataSourceFromFileNonExistent) {
    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file("/nonexistent/path/audio.wav", 48000, 2);
    EXPECT_FALSE(ds.is_valid());
}

TEST(AudioFileLoader, LoadDataSourceFromFileEmptyPath) {
    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file("", 48000, 2);
    EXPECT_FALSE(ds.is_valid());
}

TEST(AudioFileLoader, RoundTripRecordAndLoad) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_loader_roundtrip.wav";

    constexpr size_t k_frame_count = 512;
    constexpr size_t k_num_channels = 2;
    constexpr size_t k_sample_rate = 48000;

    std::array<float, k_frame_count * k_num_channels> source_data{};
    for (size_t i = 0; i < k_frame_count * k_num_channels; ++i) {
        source_data[i] = std::sin(static_cast<float>(i) * 0.05f) * 0.5f;
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count * k_num_channels> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    thl::audio_io::AudioFileLoader loader;
    auto buffer = loader.load_from_file(test_file.string(), k_sample_rate, k_num_channels);
    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.get_num_channels(), k_num_channels);
    EXPECT_EQ(buffer.get_num_frames(), k_frame_count);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), static_cast<double>(k_sample_rate));

    for (size_t ch = 0; ch < k_num_channels; ++ch) {
        const float* ptr = buffer.get_read_pointer(ch);
        for (size_t f = 0; f < k_frame_count; ++f) {
            EXPECT_FLOAT_EQ(ptr[f], source_data[f * k_num_channels + ch])
                << "Mismatch at ch=" << ch << " frame=" << f;
        }
    }

    std::filesystem::remove(test_file);
}

TEST(AudioFileLoader, RoundTripDataSource) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_loader_ds_roundtrip.wav";

    constexpr size_t k_frame_count = 256;
    constexpr size_t k_num_channels = 2;
    constexpr size_t k_sample_rate = 48000;

    std::array<float, k_frame_count * k_num_channels> source_data{};
    for (size_t i = 0; i < k_frame_count * k_num_channels; ++i) {
        source_data[i] = static_cast<float>(i) / static_cast<float>(k_frame_count * k_num_channels);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count * k_num_channels> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file(test_file.string(), k_sample_rate, k_num_channels);
    ASSERT_TRUE(ds.is_valid());
    EXPECT_EQ(ds.get_channel_count(), k_num_channels);
    EXPECT_EQ(ds.get_sample_rate(), k_sample_rate);
    EXPECT_EQ(ds.get_total_frames(), k_frame_count);
    EXPECT_EQ(ds.get_cursor(), 0u);

    std::array<float, k_frame_count * k_num_channels> read_buf{};
    uint64_t frames_read = ds.read_pcm_frames(read_buf.data(), k_frame_count);
    EXPECT_EQ(frames_read, k_frame_count);
    EXPECT_EQ(ds.get_cursor(), k_frame_count);

    for (size_t i = 0; i < k_frame_count * k_num_channels; ++i) {
        EXPECT_FLOAT_EQ(read_buf[i], source_data[i]) << "Mismatch at interleaved index " << i;
    }

    std::filesystem::remove(test_file);
}

// =============================================================================
// DataSource Tests
// =============================================================================

TEST(DataSource, InvalidDataSourceDefaults) {
    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file("", 48000, 2);
    EXPECT_FALSE(ds.is_valid());
    EXPECT_EQ(ds.get_channel_count(), 0u);
    EXPECT_EQ(ds.get_sample_rate(), 0u);
    EXPECT_EQ(ds.get_total_frames(), 0u);
    EXPECT_EQ(ds.get_cursor(), 0u);
    EXPECT_EQ(ds.read_pcm_frames(nullptr, 0), 0u);
    EXPECT_FALSE(ds.seek(0));
}

TEST(DataSource, MoveConstruction) {
    thl::audio_io::AudioFileLoader loader;
    auto ds1 = loader.load_data_source_from_file("", 48000, 2);
    auto ds2 = std::move(ds1);
    EXPECT_FALSE(ds2.is_valid());
}

TEST(DataSource, SequentialRead) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_ds_seq.wav";

    constexpr size_t k_frame_count = 512;
    constexpr size_t k_num_channels = 1;
    constexpr size_t k_sample_rate = 44100;

    std::array<float, k_frame_count> source_data{};
    for (size_t i = 0; i < k_frame_count; ++i) {
        source_data[i] = static_cast<float>(i) / static_cast<float>(k_frame_count);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file(test_file.string(), k_sample_rate, k_num_channels);
    ASSERT_TRUE(ds.is_valid());
    EXPECT_EQ(ds.get_cursor(), 0u);

    constexpr uint64_t k_chunk_size = 128;
    std::array<float, k_chunk_size> read_buf{};
    uint64_t total_read = 0;

    for (int chunk = 0; chunk < 4; ++chunk) {
        uint64_t frames_read = ds.read_pcm_frames(read_buf.data(), k_chunk_size);
        EXPECT_EQ(frames_read, k_chunk_size);

        for (uint64_t i = 0; i < frames_read; ++i) {
            EXPECT_FLOAT_EQ(read_buf[i], source_data[total_read + i])
                << "Mismatch at frame " << (total_read + i);
        }
        total_read += frames_read;
    }

    EXPECT_EQ(total_read, k_frame_count);
    EXPECT_EQ(ds.get_cursor(), k_frame_count);

    std::filesystem::remove(test_file);
}

TEST(DataSource, SeekUpdatesPosition) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_ds_seek.wav";

    constexpr size_t k_frame_count = 256;
    constexpr size_t k_num_channels = 1;
    constexpr size_t k_sample_rate = 44100;

    std::array<float, k_frame_count> source_data{};

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file(test_file.string(), k_sample_rate, k_num_channels);
    ASSERT_TRUE(ds.is_valid());

    EXPECT_TRUE(ds.seek(64));
    EXPECT_EQ(ds.get_cursor(), 64u);

    EXPECT_TRUE(ds.seek(0));
    EXPECT_EQ(ds.get_cursor(), 0u);

    std::filesystem::remove(test_file);
}

TEST(DataSource, ReadBeyondEnd) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_ds_eof.wav";

    constexpr size_t k_frame_count = 64;
    constexpr size_t k_num_channels = 1;
    constexpr size_t k_sample_rate = 44100;

    std::array<float, k_frame_count> source_data{};
    source_data[0] = 0.5f;

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file(test_file.string(), k_sample_rate, k_num_channels);
    ASSERT_TRUE(ds.is_valid());

    std::array<float, k_frame_count * 2> read_buf{};
    uint64_t frames_read = ds.read_pcm_frames(read_buf.data(), k_frame_count * 2);
    EXPECT_EQ(frames_read, k_frame_count);

    std::filesystem::remove(test_file);
}

// =============================================================================
// AudioPlayerSource Tests
// =============================================================================

TEST(AudioPlayerSource, DefaultConstruction) {
    AudioPlayerSource player;
    EXPECT_FALSE(player.is_loaded());
    EXPECT_FALSE(player.is_playing());
    EXPECT_EQ(player.get_current_frame(), 0);
    EXPECT_EQ(player.get_total_frames(), 0);
}

TEST(AudioPlayerSource, UnloadWithoutLoad) {
    AudioPlayerSource player;
    player.unload_file();
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, PlayWithoutLoad) {
    AudioPlayerSource player;
    player.play();
    EXPECT_FALSE(player.is_playing());
}

TEST(AudioPlayerSource, PauseWithoutLoad) {
    AudioPlayerSource player;
    player.pause();
    EXPECT_FALSE(player.is_playing());
}

TEST(AudioPlayerSource, StopWithoutLoad) {
    AudioPlayerSource player;
    player.stop();
    EXPECT_FALSE(player.is_playing());
}

TEST(AudioPlayerSource, SeekWithoutLoad) {
    AudioPlayerSource player;
    player.seek_to_frame(100);
    EXPECT_EQ(player.get_current_frame(), 0);
}

TEST(AudioPlayerSource, ProcessWithoutLoad) {
    AudioPlayerSource player;

    constexpr size_t k_frame_count = 256;
    constexpr size_t k_num_channels = 2;
    std::array<float, k_frame_count * k_num_channels> output{};
    std::array<float, k_frame_count * k_num_channels> input{};

    for (auto& sample : output) { sample = 1.0f; }

    player.process(output.data(), input.data(), k_frame_count, 0, k_num_channels);

    for (const auto& sample : output) { EXPECT_FLOAT_EQ(sample, 1.0f); }
}

TEST(AudioPlayerSource, LoadNonExistentFile) {
    AudioPlayerSource player;
    bool loaded = player.load_file("/nonexistent/path/audio.wav", 2, 48000);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, SetFinishedCallback) {
    AudioPlayerSource player;
    bool callback_called = false;

    player.set_finished_callback([&callback_called]() { callback_called = true; });

    EXPECT_FALSE(callback_called);
}

TEST(AudioPlayerSource, ReleaseResources) {
    AudioPlayerSource player;
    player.release_resources();
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, ProcessRejectsOutputChannelMismatch) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_player_channel_mismatch.wav";

    constexpr size_t k_frame_count = 128;
    constexpr size_t k_write_channels = 2;
    constexpr size_t k_sample_rate = 48000;
    std::array<float, k_frame_count * k_write_channels> source_data{};
    source_data[0] = 0.25f;

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_write_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count * k_write_channels> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_write_channels, 0);
        sink.close_file();
    }

    AudioPlayerSource player;
    ASSERT_TRUE(player.load_file(test_file.string(), k_write_channels, k_sample_rate));
    player.play();

    std::array<float, k_frame_count> output{};
    for (auto& sample : output) { sample = 1.0f; }
    std::array<float, k_frame_count> input{};

    player.process(output.data(), input.data(), k_frame_count, 0, 1);

    for (const auto& sample : output) { EXPECT_FLOAT_EQ(sample, 1.0f); }

    player.unload_file();
    std::filesystem::remove(test_file);
}

// Integration test: record and play back
TEST(AudioFileIntegration, RecordAndPlayback) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_record_playback.wav";

    constexpr size_t k_frame_count = 1024;
    constexpr size_t k_num_channels = 2;
    constexpr size_t k_sample_rate = 48000;

    std::array<float, k_frame_count * k_num_channels> original_data{};
    for (size_t i = 0; i < k_frame_count * k_num_channels; ++i) {
        original_data[i] = std::sin(static_cast<float>(i) * 0.1f) * 0.5f;
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();

        std::array<float, k_frame_count * k_num_channels> output{};
        sink.process(output.data(), original_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    ASSERT_TRUE(std::filesystem::exists(test_file));

    {
        AudioPlayerSource player;
        ASSERT_TRUE(player.load_file(test_file.string(), k_num_channels, k_sample_rate));
        EXPECT_TRUE(player.is_loaded());
        EXPECT_EQ(player.get_total_frames(), k_frame_count);
        EXPECT_EQ(player.get_current_frame(), 0);

        player.set_fade_enabled(false);
        player.play();
        EXPECT_TRUE(player.is_playing());

        std::array<float, k_frame_count * k_num_channels> played_data{};
        std::array<float, k_frame_count * k_num_channels> input{};

        player.process(played_data.data(), input.data(), k_frame_count, 0, k_num_channels);

        for (size_t i = 0; i < k_frame_count * k_num_channels; ++i) {
            EXPECT_FLOAT_EQ(played_data[i], original_data[i]) << "Mismatch at sample " << i;
        }

        player.unload_file();
        EXPECT_FALSE(player.is_loaded());
    }

    std::filesystem::remove(test_file);
}

// =============================================================================
// load_data_source_from_memory Tests
// =============================================================================

namespace {

std::vector<uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) { return {}; }
    auto size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

}  // namespace

TEST(AudioFileLoader, LoadDataSourceFromMemoryNullData) {
    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(nullptr, 0, 48000, 2);
    EXPECT_FALSE(ds.is_valid());
}

TEST(AudioFileLoader, LoadDataSourceFromMemoryZeroSize) {
    thl::audio_io::AudioFileLoader loader;
    std::array<uint8_t, 1> data{0};
    auto ds = loader.load_data_source_from_memory(data.data(), 0, 48000, 2);
    EXPECT_FALSE(ds.is_valid());
}

// Disabled: currently passes assertions but intermittently aborts during
// teardown in the underlying audio/memory backend on this platform.
TEST(AudioFileLoader, LoadDataSourceFromMemoryInvalidData) {
    thl::audio_io::AudioFileLoader loader;
    std::array<uint8_t, 4> garbage{0xDE, 0xAD, 0xBE, 0xEF};
    auto ds = loader.load_data_source_from_memory(garbage.data(), garbage.size(), 48000, 2);
    EXPECT_FALSE(ds.is_valid());
}

TEST(AudioFileLoader, LoadDataSourceFromMemoryRoundTrip) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_ds_from_memory_roundtrip.wav";

    constexpr size_t k_frame_count = 256;
    constexpr size_t k_num_channels = 2;
    constexpr size_t k_sample_rate = 48000;

    std::array<float, k_frame_count * k_num_channels> source_data{};
    for (size_t i = 0; i < k_frame_count * k_num_channels; ++i) {
        source_data[i] = static_cast<float>(i) / static_cast<float>(k_frame_count * k_num_channels);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count * k_num_channels> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    auto wav_bytes = read_file_bytes(test_file);
    ASSERT_FALSE(wav_bytes.empty());
    std::filesystem::remove(test_file);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wav_bytes.data(),
                                                  wav_bytes.size(),
                                                  k_sample_rate,
                                                  k_num_channels);
    ASSERT_TRUE(ds.is_valid());
    EXPECT_EQ(ds.get_channel_count(), k_num_channels);
    EXPECT_EQ(ds.get_sample_rate(), k_sample_rate);
    EXPECT_EQ(ds.get_total_frames(), k_frame_count);
    EXPECT_EQ(ds.get_cursor(), 0u);

    std::array<float, k_frame_count * k_num_channels> read_buf{};
    uint64_t frames_read = ds.read_pcm_frames(read_buf.data(), k_frame_count);
    EXPECT_EQ(frames_read, k_frame_count);
    EXPECT_EQ(ds.get_cursor(), k_frame_count);

    for (size_t i = 0; i < k_frame_count * k_num_channels; ++i) {
        EXPECT_FLOAT_EQ(read_buf[i], source_data[i]) << "Mismatch at interleaved index " << i;
    }
}

TEST(AudioFileLoader, LoadDataSourceFromMemorySeek) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_ds_mem_seek.wav";

    constexpr size_t k_frame_count = 256;
    constexpr size_t k_num_channels = 1;
    constexpr size_t k_sample_rate = 44100;

    std::array<float, k_frame_count> source_data{};
    for (size_t i = 0; i < k_frame_count; ++i) {
        source_data[i] = static_cast<float>(i) / static_cast<float>(k_frame_count);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    auto wav_bytes = read_file_bytes(test_file);
    ASSERT_FALSE(wav_bytes.empty());
    std::filesystem::remove(test_file);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wav_bytes.data(),
                                                  wav_bytes.size(),
                                                  k_sample_rate,
                                                  k_num_channels);
    ASSERT_TRUE(ds.is_valid());

    EXPECT_TRUE(ds.seek(128));
    EXPECT_EQ(ds.get_cursor(), 128u);

    std::array<float, 128> read_buf{};
    uint64_t frames_read = ds.read_pcm_frames(read_buf.data(), 128);
    EXPECT_EQ(frames_read, 128u);

    for (uint32_t i = 0; i < 128; ++i) {
        EXPECT_FLOAT_EQ(read_buf[i], source_data[128 + i]) << "Mismatch at frame " << (128 + i);
    }

    EXPECT_TRUE(ds.seek(0));
    EXPECT_EQ(ds.get_cursor(), 0u);
}

TEST(AudioFileLoader, LoadDataSourceFromMemorySequentialRead) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_ds_mem_seq.wav";

    constexpr size_t k_frame_count = 512;
    constexpr size_t k_num_channels = 1;
    constexpr size_t k_sample_rate = 44100;

    std::array<float, k_frame_count> source_data{};
    for (size_t i = 0; i < k_frame_count; ++i) {
        source_data[i] = std::sin(static_cast<float>(i) * 0.1f) * 0.5f;
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    auto wav_bytes = read_file_bytes(test_file);
    ASSERT_FALSE(wav_bytes.empty());
    std::filesystem::remove(test_file);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wav_bytes.data(),
                                                  wav_bytes.size(),
                                                  k_sample_rate,
                                                  k_num_channels);
    ASSERT_TRUE(ds.is_valid());

    constexpr uint64_t k_chunk_size = 128;
    std::array<float, k_chunk_size> read_buf{};
    uint64_t total_read = 0;

    for (int chunk = 0; chunk < 4; ++chunk) {
        uint64_t frames_read = ds.read_pcm_frames(read_buf.data(), k_chunk_size);
        EXPECT_EQ(frames_read, k_chunk_size);

        for (uint64_t i = 0; i < frames_read; ++i) {
            EXPECT_FLOAT_EQ(read_buf[i], source_data[total_read + i])
                << "Mismatch at frame " << (total_read + i);
        }
        total_read += frames_read;
    }

    EXPECT_EQ(total_read, k_frame_count);
}

TEST(AudioFileLoader, LoadDataSourceFromMemoryNativeFormat) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_ds_mem_native.wav";

    constexpr size_t k_frame_count = 128;
    constexpr size_t k_num_channels = 2;
    constexpr size_t k_sample_rate = 44100;

    std::array<float, k_frame_count * k_num_channels> source_data{};
    for (auto& val : source_data) { val = 0.25f; }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count * k_num_channels> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    auto wav_bytes = read_file_bytes(test_file);
    ASSERT_FALSE(wav_bytes.empty());
    std::filesystem::remove(test_file);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wav_bytes.data(), wav_bytes.size());
    ASSERT_TRUE(ds.is_valid());
    EXPECT_EQ(ds.get_channel_count(), k_num_channels);
    EXPECT_EQ(ds.get_sample_rate(), k_sample_rate);
    EXPECT_EQ(ds.get_total_frames(), k_frame_count);
}

// =============================================================================
// AudioPlayerSource::loadFromMemory Tests
// =============================================================================

TEST(AudioPlayerSource, LoadFromMemoryNullData) {
    AudioPlayerSource player;
    bool loaded = player.load_from_memory(nullptr, 0, 2, 48000);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, LoadFromMemoryZeroSize) {
    AudioPlayerSource player;
    std::array<uint8_t, 1> data{0};
    bool loaded = player.load_from_memory(data.data(), 0, 2, 48000);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, LoadFromMemoryZeroChannels) {
    AudioPlayerSource player;
    std::array<uint8_t, 1> data{0};
    bool loaded = player.load_from_memory(data.data(), 1, 0, 48000);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, LoadFromMemoryZeroSampleRate) {
    AudioPlayerSource player;
    std::array<uint8_t, 1> data{0};
    bool loaded = player.load_from_memory(data.data(), 1, 2, 0);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(player.is_loaded());
}

// Disabled: currently passes assertions but intermittently aborts during
// teardown in the underlying audio/memory backend on this platform.
TEST(AudioPlayerSource, LoadFromMemoryInvalidData) {
    AudioPlayerSource player;
    std::array<uint8_t, 4> garbage{0xDE, 0xAD, 0xBE, 0xEF};
    bool loaded = player.load_from_memory(garbage.data(), garbage.size(), 2, 48000);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, LoadFromMemoryAndPlayback) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_player_from_memory.wav";

    constexpr size_t k_frame_count = 512;
    constexpr size_t k_num_channels = 2;
    constexpr size_t k_sample_rate = 48000;

    std::array<float, k_frame_count * k_num_channels> original_data{};
    for (size_t i = 0; i < k_frame_count * k_num_channels; ++i) {
        original_data[i] = std::sin(static_cast<float>(i) * 0.1f) * 0.5f;
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count * k_num_channels> sink_output{};
        sink.process(sink_output.data(), original_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    auto wav_bytes = read_file_bytes(test_file);
    ASSERT_FALSE(wav_bytes.empty());
    std::filesystem::remove(test_file);

    AudioPlayerSource player;
    ASSERT_TRUE(
        player.load_from_memory(wav_bytes.data(), wav_bytes.size(), k_num_channels, k_sample_rate));
    EXPECT_TRUE(player.is_loaded());
    EXPECT_EQ(player.get_total_frames(), k_frame_count);
    EXPECT_EQ(player.get_current_frame(), 0u);

    player.set_fade_enabled(false);
    player.play();
    EXPECT_TRUE(player.is_playing());

    std::array<float, k_frame_count * k_num_channels> played_data{};
    std::array<float, k_frame_count * k_num_channels> input{};
    player.process(played_data.data(), input.data(), k_frame_count, 0, k_num_channels);

    for (size_t i = 0; i < k_frame_count * k_num_channels; ++i) {
        EXPECT_FLOAT_EQ(played_data[i], original_data[i]) << "Mismatch at sample " << i;
    }

    player.unload_file();
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, LoadFromMemorySeek) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_player_from_memory_seek.wav";

    constexpr uint32_t k_frame_count = 256;
    constexpr uint32_t k_num_channels = 1;
    constexpr uint32_t k_sample_rate = 44100;

    std::array<float, k_frame_count> source_data{};
    for (uint32_t i = 0; i < k_frame_count; ++i) {
        source_data[i] = static_cast<float>(i) / static_cast<float>(k_frame_count);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    auto wav_bytes = read_file_bytes(test_file);
    ASSERT_FALSE(wav_bytes.empty());
    std::filesystem::remove(test_file);

    AudioPlayerSource player;
    ASSERT_TRUE(
        player.load_from_memory(wav_bytes.data(), wav_bytes.size(), k_num_channels, k_sample_rate));

    player.seek_to_frame(128);
    EXPECT_EQ(player.get_current_frame(), 128u);

    player.set_fade_enabled(false);
    player.play();

    std::array<float, 128> read_buf{};
    std::array<float, 128> input{};
    player.process(read_buf.data(), input.data(), 128, 0, k_num_channels);

    for (uint32_t i = 0; i < 128u; ++i) {
        EXPECT_FLOAT_EQ(read_buf[i], source_data[128 + i]) << "Mismatch at sample " << i;
    }

    player.unload_file();
}

TEST(AudioPlayerSource, LoadFromMemoryStopResetsPosition) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_player_from_memory_stop.wav";

    constexpr uint32_t k_frame_count = 128;
    constexpr uint32_t k_num_channels = 1;
    constexpr uint32_t k_sample_rate = 44100;

    std::array<float, k_frame_count> source_data{};

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    auto wav_bytes = read_file_bytes(test_file);
    ASSERT_FALSE(wav_bytes.empty());
    std::filesystem::remove(test_file);

    AudioPlayerSource player;
    ASSERT_TRUE(
        player.load_from_memory(wav_bytes.data(), wav_bytes.size(), k_num_channels, k_sample_rate));
    player.play();

    std::array<float, 64> read_buf{};
    std::array<float, 64> input{};
    player.process(read_buf.data(), input.data(), 64, 0, k_num_channels);
    EXPECT_EQ(player.get_current_frame(), 64u);

    player.stop();
    EXPECT_FALSE(player.is_playing());
    EXPECT_EQ(player.get_current_frame(), 0u);

    player.unload_file();
}

TEST(AudioPlayerSource, LoadFromMemoryReplacesFile) {
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path();
    std::filesystem::path test_file = temp_dir / "test_player_mem_replaces_file.wav";

    constexpr uint32_t k_frame_count = 128;
    constexpr uint32_t k_num_channels = 1;
    constexpr uint32_t k_sample_rate = 44100;

    std::array<float, k_frame_count> source_data{};
    for (float& s : source_data) { s = 0.5f; }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(test_file.string(), k_num_channels, k_sample_rate));
        sink.start_recording();
        std::array<float, k_frame_count> sink_output{};
        sink.process(sink_output.data(), source_data.data(), k_frame_count, k_num_channels, 0);
        sink.close_file();
    }

    auto wav_bytes = read_file_bytes(test_file);
    ASSERT_FALSE(wav_bytes.empty());

    AudioPlayerSource player;
    ASSERT_TRUE(player.load_file(test_file.string(), k_num_channels, k_sample_rate));
    EXPECT_TRUE(player.is_loaded());

    ASSERT_TRUE(
        player.load_from_memory(wav_bytes.data(), wav_bytes.size(), k_num_channels, k_sample_rate));
    EXPECT_TRUE(player.is_loaded());
    EXPECT_EQ(player.get_total_frames(), k_frame_count);

    player.unload_file();
    std::filesystem::remove(test_file);
}
