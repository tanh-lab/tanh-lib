#include <gtest/gtest.h>
#include "tanh/audio-io/AudioDeviceInfo.h"
#include "tanh/audio-io/AudioDeviceManager.h"
#include "tanh/audio-io/AudioFileLoader.h"
#include "tanh/audio-io/AudioFileSink.h"
#include "tanh/audio-io/AudioIODeviceCallback.h"
#include "tanh/audio-io/AudioPlayerSource.h"
#include "tanh/audio-io/DataSource.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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
    auto inputDevices = manager.enumerate_input_devices();
    auto outputDevices = manager.enumerate_output_devices();

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
    int prepareCallCount = 0;
    int releaseCallCount = 0;
    int processCallCount = 0;

    void process(float* outputBuffer,
                 const float* inputBuffer,
                 uint32_t frameCount,
                 uint32_t /*numInputChannels*/,
                 uint32_t numOutputChannels) override {
        ++processCallCount;
        // Fill output with silence
        if (!outputBuffer || numOutputChannels == 0) { return; }
        for (uint32_t i = 0; i < frameCount * numOutputChannels; ++i) { outputBuffer[i] = 0.0f; }
        (void)inputBuffer;
    }

    void prepare_to_play(uint32_t sampleRate, uint32_t bufferSize) override {
        ++prepareCallCount;
        (void)sampleRate;
        (void)bufferSize;
    }

    void release_resources() override { ++releaseCallCount; }
};

TEST(AudioIODeviceCallback, MockCallbackConstruction) {
    MockCallback callback;
    EXPECT_EQ(callback.prepareCallCount, 0);
    EXPECT_EQ(callback.releaseCallCount, 0);
    EXPECT_EQ(callback.processCallCount, 0);
}

TEST(AudioIODeviceCallback, ProcessFillsSilence) {
    MockCallback callback;

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 2;
    float output[frameCount * numChannels];
    float input[frameCount * numChannels] = {0};

    // Fill output with non-zero values
    for (auto& sample : output) { sample = 1.0f; }

    callback.process(output, input, frameCount, numChannels, numChannels);

    EXPECT_EQ(callback.processCallCount, 1);

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

    constexpr int kNumCallbacks = 8;
    MockCallback callbacks[kNumCallbacks];

    std::atomic<bool> running{true};
    std::atomic<int> addCount{0};
    std::atomic<int> removeCount{0};

    // Multiple threads adding/removing callbacks concurrently
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            while (running.load(std::memory_order_relaxed)) {
                int idx = (t * 2) % kNumCallbacks;
                manager.add_playback_callback(&callbacks[idx]);
                addCount.fetch_add(1, std::memory_order_relaxed);

                std::this_thread::sleep_for(std::chrono::microseconds(50));

                manager.remove_playback_callback(&callbacks[idx]);
                removeCount.fetch_add(1, std::memory_order_relaxed);

                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }

    // Let threads run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running.store(false, std::memory_order_relaxed);

    for (auto& thread : threads) { thread.join(); }

    // Verify operations completed without crashing
    EXPECT_GT(addCount.load(), 0);
    EXPECT_GT(removeCount.load(), 0);
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
    uint32_t maxFrames = static_cast<uint32_t>(48000 * 0.064f);
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(4096, 48000), maxFrames);
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(3072, 48000), 3072u);
}

TEST(AudioDeviceManager, BluetoothClampAt44100Hz) {
    uint32_t maxFrames = static_cast<uint32_t>(44100 * 0.064f);
    EXPECT_EQ(AudioDeviceManager::clamp_buffer_size_for_bluetooth_route(4096, 44100), maxFrames);
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
    std::atomic<int> processCallCount{0};
    double frequency = 440.0;
    double phase = 0.0;
    double sampleRate = 48000.0;

    explicit SineCallback(double freq = 440.0) : frequency(freq) {}

    void process(float* outputBuffer,
                 const float* /*inputBuffer*/,
                 uint32_t frameCount,
                 uint32_t /*numInputChannels*/,
                 uint32_t numOutputChannels) override {
        ++processCallCount;
        if (!outputBuffer || numOutputChannels == 0) { return; }

        double phaseIncrement = (2.0 * M_PI * frequency) / sampleRate;

        for (uint32_t frame = 0; frame < frameCount; ++frame) {
            float sample = static_cast<float>(std::sin(phase) * 0.3);
            phase += phaseIncrement;
            if (phase >= 2.0 * M_PI) { phase -= 2.0 * M_PI; }

            for (uint32_t ch = 0; ch < numOutputChannels; ++ch) {
                outputBuffer[frame * numOutputChannels + ch] = sample;
            }
        }
    }

    void prepare_to_play(uint32_t rate, uint32_t /*bufferSize*/) override {
        sampleRate = static_cast<double>(rate);
        phase = 0.0;
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
            if (i > 0) std::cout << ", ";
            std::cout << device.m_sample_rates[i];
        }
        std::cout << ")\n";
    }

    std::cout << "\n=== Output Devices (" << outputs.size() << ") ===\n";
    for (const auto& device : outputs) {
        std::cout << "  - " << device.m_name << " (rates: ";
        for (size_t i = 0; i < device.m_sample_rates.size(); ++i) {
            if (i > 0) std::cout << ", ";
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

    SineCallback sineCallback(440.0);

    bool initialised = manager.initialise(nullptr, &outputs[0], 48000, 256, 0, 2);
    ASSERT_TRUE(initialised) << "Failed to initialise audio device";

    manager.add_playback_callback(&sineCallback);

    ASSERT_TRUE(manager.start_playback()) << "Failed to start audio";
    EXPECT_TRUE(manager.is_playback_running());

    std::cout << "Playing 440Hz sine wave for 500ms...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    manager.stop_playback();
    EXPECT_FALSE(manager.is_playback_running());

    std::cout << "Process callback was called " << sineCallback.processCallCount << " times\n";

    EXPECT_GT(sineCallback.processCallCount.load(), 0) << "Process callback was never called";
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

        EXPECT_GT(callback.processCallCount.load(), 0);
    }

    std::cout << "Total process calls: " << callback.processCallCount << "\n";
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

    std::cout << "Duplex process calls: " << callback.processCallCount << "\n";
    EXPECT_GT(callback.processCallCount, 0);
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

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 2;
    float output[frameCount * numChannels] = {0};
    float input[frameCount * numChannels] = {0};

    sink.process(output, input, frameCount, numChannels, 0);
    EXPECT_EQ(sink.get_frames_written(), 0);
}

TEST(AudioFileSink, OpenAndCloseFile) {
    AudioFileSink sink;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_audio_sink.wav";

    bool opened = sink.open_file(testFile.string(), 2, 48000);
    EXPECT_TRUE(opened);
    EXPECT_TRUE(sink.is_open());
    EXPECT_FALSE(sink.is_recording());

    sink.close_file();
    EXPECT_FALSE(sink.is_open());

    std::filesystem::remove(testFile);
}

TEST(AudioFileSink, RecordingStateToggle) {
    AudioFileSink sink;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_audio_sink_toggle.wav";

    ASSERT_TRUE(sink.open_file(testFile.string(), 2, 48000));

    EXPECT_FALSE(sink.is_recording());
    sink.start_recording();
    EXPECT_TRUE(sink.is_recording());
    sink.stop_recording();
    EXPECT_FALSE(sink.is_recording());

    sink.close_file();
    std::filesystem::remove(testFile);
}

TEST(AudioFileSink, WriteFrames) {
    AudioFileSink sink;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_audio_sink_write.wav";

    ASSERT_TRUE(sink.open_file(testFile.string(), 2, 48000));
    sink.start_recording();

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 2;
    float output[frameCount * numChannels] = {0};
    float input[frameCount * numChannels];

    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        input[i] = static_cast<float>(i) / static_cast<float>(frameCount * numChannels);
    }

    sink.process(output, input, frameCount, numChannels, 0);
    EXPECT_EQ(sink.get_frames_written(), frameCount);

    sink.process(output, input, frameCount, numChannels, 0);
    EXPECT_EQ(sink.get_frames_written(), frameCount * 2);

    sink.close_file();
    EXPECT_TRUE(std::filesystem::exists(testFile));
    std::filesystem::remove(testFile);
}

TEST(AudioFileSink, RejectMismatchedInputChannels) {
    AudioFileSink sink;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_audio_sink_channel_mismatch.wav";

    ASSERT_TRUE(sink.open_file(testFile.string(), 2, 48000));
    sink.start_recording();

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t inputChannels = 1;
    float output[frameCount * 2] = {0};
    float input[frameCount * inputChannels] = {0};

    sink.process(output, input, frameCount, inputChannels, 0);
    EXPECT_EQ(sink.get_frames_written(), 0);

    sink.close_file();
    std::filesystem::remove(testFile);
}

TEST(AudioFileSink, ReleaseResources) {
    AudioFileSink sink;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_audio_sink_release.wav";

    ASSERT_TRUE(sink.open_file(testFile.string(), 2, 48000));
    sink.start_recording();

    sink.release_resources();
    EXPECT_FALSE(sink.is_open());
    EXPECT_FALSE(sink.is_recording());

    std::filesystem::remove(testFile);
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
    uint8_t data[] = {0};
    auto buffer = loader.load_from_memory(data, 0, 48000, 2);
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
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_loader_roundtrip.wav";

    constexpr uint32_t frameCount = 512;
    constexpr uint32_t numChannels = 2;
    constexpr uint32_t sampleRate = 48000;

    float sourceData[frameCount * numChannels];
    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        sourceData[i] = std::sin(static_cast<float>(i) * 0.05f) * 0.5f;
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount * numChannels] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    thl::audio_io::AudioFileLoader loader;
    auto buffer = loader.load_from_file(testFile.string(), sampleRate, numChannels);
    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.get_num_channels(), numChannels);
    EXPECT_EQ(buffer.get_num_frames(), frameCount);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), static_cast<double>(sampleRate));

    for (uint32_t ch = 0; ch < numChannels; ++ch) {
        const float* ptr = buffer.get_read_pointer(ch);
        for (uint32_t f = 0; f < frameCount; ++f) {
            EXPECT_FLOAT_EQ(ptr[f], sourceData[f * numChannels + ch])
                << "Mismatch at ch=" << ch << " frame=" << f;
        }
    }

    std::filesystem::remove(testFile);
}

TEST(AudioFileLoader, RoundTripDataSource) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_loader_ds_roundtrip.wav";

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 2;
    constexpr uint32_t sampleRate = 48000;

    float sourceData[frameCount * numChannels];
    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        sourceData[i] = static_cast<float>(i) / static_cast<float>(frameCount * numChannels);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount * numChannels] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file(testFile.string(), sampleRate, numChannels);
    ASSERT_TRUE(ds.is_valid());
    EXPECT_EQ(ds.get_channel_count(), numChannels);
    EXPECT_EQ(ds.get_sample_rate(), sampleRate);
    EXPECT_EQ(ds.get_total_frames(), frameCount);
    EXPECT_EQ(ds.get_cursor(), 0u);

    float readBuf[frameCount * numChannels] = {0};
    uint64_t framesRead = ds.read_pcm_frames(readBuf, frameCount);
    EXPECT_EQ(framesRead, frameCount);
    EXPECT_EQ(ds.get_cursor(), frameCount);

    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        EXPECT_FLOAT_EQ(readBuf[i], sourceData[i]) << "Mismatch at interleaved index " << i;
    }

    std::filesystem::remove(testFile);
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
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_ds_seq.wav";

    constexpr uint32_t frameCount = 512;
    constexpr uint32_t numChannels = 1;
    constexpr uint32_t sampleRate = 44100;

    float sourceData[frameCount];
    for (uint32_t i = 0; i < frameCount; ++i) {
        sourceData[i] = static_cast<float>(i) / static_cast<float>(frameCount);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file(testFile.string(), sampleRate, numChannels);
    ASSERT_TRUE(ds.is_valid());
    EXPECT_EQ(ds.get_cursor(), 0u);

    constexpr uint64_t chunkSize = 128;
    float readBuf[chunkSize] = {0};
    uint64_t totalRead = 0;

    for (int chunk = 0; chunk < 4; ++chunk) {
        uint64_t framesRead = ds.read_pcm_frames(readBuf, chunkSize);
        EXPECT_EQ(framesRead, chunkSize);

        for (uint64_t i = 0; i < framesRead; ++i) {
            EXPECT_FLOAT_EQ(readBuf[i], sourceData[totalRead + i])
                << "Mismatch at frame " << (totalRead + i);
        }
        totalRead += framesRead;
    }

    EXPECT_EQ(totalRead, frameCount);
    EXPECT_EQ(ds.get_cursor(), frameCount);

    std::filesystem::remove(testFile);
}

TEST(DataSource, SeekUpdatesPosition) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_ds_seek.wav";

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 1;
    constexpr uint32_t sampleRate = 44100;

    float sourceData[frameCount] = {0};

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file(testFile.string(), sampleRate, numChannels);
    ASSERT_TRUE(ds.is_valid());

    EXPECT_TRUE(ds.seek(64));
    EXPECT_EQ(ds.get_cursor(), 64u);

    EXPECT_TRUE(ds.seek(0));
    EXPECT_EQ(ds.get_cursor(), 0u);

    std::filesystem::remove(testFile);
}

TEST(DataSource, ReadBeyondEnd) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_ds_eof.wav";

    constexpr uint32_t frameCount = 64;
    constexpr uint32_t numChannels = 1;
    constexpr uint32_t sampleRate = 44100;

    float sourceData[frameCount] = {0.5f};

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file(testFile.string(), sampleRate, numChannels);
    ASSERT_TRUE(ds.is_valid());

    float readBuf[frameCount * 2] = {0};
    uint64_t framesRead = ds.read_pcm_frames(readBuf, frameCount * 2);
    EXPECT_EQ(framesRead, frameCount);

    std::filesystem::remove(testFile);
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

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 2;
    float output[frameCount * numChannels];
    float input[frameCount * numChannels] = {0};

    for (auto& sample : output) { sample = 1.0f; }

    player.process(output, input, frameCount, 0, numChannels);

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
    bool callbackCalled = false;

    player.set_finished_callback([&callbackCalled]() { callbackCalled = true; });

    EXPECT_FALSE(callbackCalled);
}

TEST(AudioPlayerSource, ReleaseResources) {
    AudioPlayerSource player;
    player.release_resources();
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, ProcessRejectsOutputChannelMismatch) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_player_channel_mismatch.wav";

    constexpr uint32_t frameCount = 128;
    constexpr uint32_t writeChannels = 2;
    constexpr uint32_t sampleRate = 48000;
    float sourceData[frameCount * writeChannels] = {0.25f};

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), writeChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount * writeChannels] = {0};
        sink.process(sinkOutput, sourceData, frameCount, writeChannels, 0);
        sink.close_file();
    }

    AudioPlayerSource player;
    ASSERT_TRUE(player.load_file(testFile.string(), writeChannels, sampleRate));
    player.play();

    float output[frameCount] = {};
    for (auto& sample : output) { sample = 1.0f; }
    float input[frameCount] = {0};

    player.process(output, input, frameCount, 0, 1);

    for (const auto& sample : output) { EXPECT_FLOAT_EQ(sample, 1.0f); }

    player.unload_file();
    std::filesystem::remove(testFile);
}

// Integration test: record and play back
TEST(AudioFileIntegration, RecordAndPlayback) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_record_playback.wav";

    constexpr uint32_t frameCount = 1024;
    constexpr uint32_t numChannels = 2;
    constexpr uint32_t sampleRate = 48000;

    float originalData[frameCount * numChannels];
    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        originalData[i] = std::sin(static_cast<float>(i) * 0.1f) * 0.5f;
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();

        float output[frameCount * numChannels] = {0};
        sink.process(output, originalData, frameCount, numChannels, 0);
        sink.close_file();
    }

    ASSERT_TRUE(std::filesystem::exists(testFile));

    {
        AudioPlayerSource player;
        ASSERT_TRUE(player.load_file(testFile.string(), numChannels, sampleRate));
        EXPECT_TRUE(player.is_loaded());
        EXPECT_EQ(player.get_total_frames(), frameCount);
        EXPECT_EQ(player.get_current_frame(), 0);

        player.set_fade_enabled(false);
        player.play();
        EXPECT_TRUE(player.is_playing());

        float playedData[frameCount * numChannels] = {0};
        float input[frameCount * numChannels] = {0};

        player.process(playedData, input, frameCount, 0, numChannels);

        for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
            EXPECT_FLOAT_EQ(playedData[i], originalData[i]) << "Mismatch at sample " << i;
        }

        player.unload_file();
        EXPECT_FALSE(player.is_loaded());
    }

    std::filesystem::remove(testFile);
}

// =============================================================================
// load_data_source_from_memory Tests
// =============================================================================

namespace {

std::vector<uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};
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
    uint8_t data[] = {0};
    auto ds = loader.load_data_source_from_memory(data, 0, 48000, 2);
    EXPECT_FALSE(ds.is_valid());
}

// Disabled: currently passes assertions but intermittently aborts during
// teardown in the underlying audio/memory backend on this platform.
TEST(AudioFileLoader, LoadDataSourceFromMemoryInvalidData) {
    thl::audio_io::AudioFileLoader loader;
    uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto ds = loader.load_data_source_from_memory(garbage, sizeof(garbage), 48000, 2);
    EXPECT_FALSE(ds.is_valid());
}

TEST(AudioFileLoader, LoadDataSourceFromMemoryRoundTrip) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_ds_from_memory_roundtrip.wav";

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 2;
    constexpr uint32_t sampleRate = 48000;

    float sourceData[frameCount * numChannels];
    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        sourceData[i] = static_cast<float>(i) / static_cast<float>(frameCount * numChannels);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount * numChannels] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());
    std::filesystem::remove(testFile);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wavBytes.data(),
                                                  wavBytes.size(),
                                                  sampleRate,
                                                  numChannels);
    ASSERT_TRUE(ds.is_valid());
    EXPECT_EQ(ds.get_channel_count(), numChannels);
    EXPECT_EQ(ds.get_sample_rate(), sampleRate);
    EXPECT_EQ(ds.get_total_frames(), frameCount);
    EXPECT_EQ(ds.get_cursor(), 0u);

    float readBuf[frameCount * numChannels] = {0};
    uint64_t framesRead = ds.read_pcm_frames(readBuf, frameCount);
    EXPECT_EQ(framesRead, frameCount);
    EXPECT_EQ(ds.get_cursor(), frameCount);

    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        EXPECT_FLOAT_EQ(readBuf[i], sourceData[i]) << "Mismatch at interleaved index " << i;
    }
}

TEST(AudioFileLoader, LoadDataSourceFromMemorySeek) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_ds_mem_seek.wav";

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 1;
    constexpr uint32_t sampleRate = 44100;

    float sourceData[frameCount];
    for (uint32_t i = 0; i < frameCount; ++i) {
        sourceData[i] = static_cast<float>(i) / static_cast<float>(frameCount);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());
    std::filesystem::remove(testFile);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wavBytes.data(),
                                                  wavBytes.size(),
                                                  sampleRate,
                                                  numChannels);
    ASSERT_TRUE(ds.is_valid());

    EXPECT_TRUE(ds.seek(128));
    EXPECT_EQ(ds.get_cursor(), 128u);

    float readBuf[128] = {0};
    uint64_t framesRead = ds.read_pcm_frames(readBuf, 128);
    EXPECT_EQ(framesRead, 128u);

    for (uint32_t i = 0; i < 128; ++i) {
        EXPECT_FLOAT_EQ(readBuf[i], sourceData[128 + i]) << "Mismatch at frame " << (128 + i);
    }

    EXPECT_TRUE(ds.seek(0));
    EXPECT_EQ(ds.get_cursor(), 0u);
}

TEST(AudioFileLoader, LoadDataSourceFromMemorySequentialRead) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_ds_mem_seq.wav";

    constexpr uint32_t frameCount = 512;
    constexpr uint32_t numChannels = 1;
    constexpr uint32_t sampleRate = 44100;

    float sourceData[frameCount];
    for (uint32_t i = 0; i < frameCount; ++i) {
        sourceData[i] = std::sin(static_cast<float>(i) * 0.1f) * 0.5f;
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());
    std::filesystem::remove(testFile);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wavBytes.data(),
                                                  wavBytes.size(),
                                                  sampleRate,
                                                  numChannels);
    ASSERT_TRUE(ds.is_valid());

    constexpr uint64_t chunkSize = 128;
    float readBuf[chunkSize] = {0};
    uint64_t totalRead = 0;

    for (int chunk = 0; chunk < 4; ++chunk) {
        uint64_t framesRead = ds.read_pcm_frames(readBuf, chunkSize);
        EXPECT_EQ(framesRead, chunkSize);

        for (uint64_t i = 0; i < framesRead; ++i) {
            EXPECT_FLOAT_EQ(readBuf[i], sourceData[totalRead + i])
                << "Mismatch at frame " << (totalRead + i);
        }
        totalRead += framesRead;
    }

    EXPECT_EQ(totalRead, frameCount);
}

TEST(AudioFileLoader, LoadDataSourceFromMemoryNativeFormat) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_ds_mem_native.wav";

    constexpr uint32_t frameCount = 128;
    constexpr uint32_t numChannels = 2;
    constexpr uint32_t sampleRate = 44100;

    float sourceData[frameCount * numChannels] = {};
    for (uint32_t i = 0; i < frameCount * numChannels; ++i) { sourceData[i] = 0.25f; }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount * numChannels] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());
    std::filesystem::remove(testFile);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wavBytes.data(), wavBytes.size());
    ASSERT_TRUE(ds.is_valid());
    EXPECT_EQ(ds.get_channel_count(), numChannels);
    EXPECT_EQ(ds.get_sample_rate(), sampleRate);
    EXPECT_EQ(ds.get_total_frames(), frameCount);
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
    uint8_t data[] = {0};
    bool loaded = player.load_from_memory(data, 0, 2, 48000);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, LoadFromMemoryZeroChannels) {
    AudioPlayerSource player;
    uint8_t data[] = {0};
    bool loaded = player.load_from_memory(data, 1, 0, 48000);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, LoadFromMemoryZeroSampleRate) {
    AudioPlayerSource player;
    uint8_t data[] = {0};
    bool loaded = player.load_from_memory(data, 1, 2, 0);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(player.is_loaded());
}

// Disabled: currently passes assertions but intermittently aborts during
// teardown in the underlying audio/memory backend on this platform.
TEST(AudioPlayerSource, LoadFromMemoryInvalidData) {
    AudioPlayerSource player;
    uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF};
    bool loaded = player.load_from_memory(garbage, sizeof(garbage), 2, 48000);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, LoadFromMemoryAndPlayback) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_player_from_memory.wav";

    constexpr uint32_t frameCount = 512;
    constexpr uint32_t numChannels = 2;
    constexpr uint32_t sampleRate = 48000;

    float originalData[frameCount * numChannels];
    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        originalData[i] = std::sin(static_cast<float>(i) * 0.1f) * 0.5f;
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount * numChannels] = {0};
        sink.process(sinkOutput, originalData, frameCount, numChannels, 0);
        sink.close_file();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());
    std::filesystem::remove(testFile);

    AudioPlayerSource player;
    ASSERT_TRUE(player.load_from_memory(wavBytes.data(), wavBytes.size(), numChannels, sampleRate));
    EXPECT_TRUE(player.is_loaded());
    EXPECT_EQ(player.get_total_frames(), frameCount);
    EXPECT_EQ(player.get_current_frame(), 0u);

    player.set_fade_enabled(false);
    player.play();
    EXPECT_TRUE(player.is_playing());

    float playedData[frameCount * numChannels] = {0};
    float input[frameCount * numChannels] = {0};
    player.process(playedData, input, frameCount, 0, numChannels);

    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        EXPECT_FLOAT_EQ(playedData[i], originalData[i]) << "Mismatch at sample " << i;
    }

    player.unload_file();
    EXPECT_FALSE(player.is_loaded());
}

TEST(AudioPlayerSource, LoadFromMemorySeek) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_player_from_memory_seek.wav";

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 1;
    constexpr uint32_t sampleRate = 44100;

    float sourceData[frameCount];
    for (uint32_t i = 0; i < frameCount; ++i) {
        sourceData[i] = static_cast<float>(i) / static_cast<float>(frameCount);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());
    std::filesystem::remove(testFile);

    AudioPlayerSource player;
    ASSERT_TRUE(player.load_from_memory(wavBytes.data(), wavBytes.size(), numChannels, sampleRate));

    player.seek_to_frame(128);
    EXPECT_EQ(player.get_current_frame(), 128u);

    player.set_fade_enabled(false);
    player.play();

    float readBuf[128] = {0};
    float input[128] = {0};
    player.process(readBuf, input, 128, 0, numChannels);

    for (uint32_t i = 0; i < 128u; ++i) {
        EXPECT_FLOAT_EQ(readBuf[i], sourceData[128 + i]) << "Mismatch at sample " << i;
    }

    player.unload_file();
}

TEST(AudioPlayerSource, LoadFromMemoryStopResetsPosition) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_player_from_memory_stop.wav";

    constexpr uint32_t frameCount = 128;
    constexpr uint32_t numChannels = 1;
    constexpr uint32_t sampleRate = 44100;

    float sourceData[frameCount] = {};

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());
    std::filesystem::remove(testFile);

    AudioPlayerSource player;
    ASSERT_TRUE(player.load_from_memory(wavBytes.data(), wavBytes.size(), numChannels, sampleRate));
    player.play();

    float readBuf[64] = {0};
    float input[64] = {0};
    player.process(readBuf, input, 64, 0, numChannels);
    EXPECT_EQ(player.get_current_frame(), 64u);

    player.stop();
    EXPECT_FALSE(player.is_playing());
    EXPECT_EQ(player.get_current_frame(), 0u);

    player.unload_file();
}

TEST(AudioPlayerSource, LoadFromMemoryReplacesFile) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_player_mem_replaces_file.wav";

    constexpr uint32_t frameCount = 128;
    constexpr uint32_t numChannels = 1;
    constexpr uint32_t sampleRate = 44100;

    float sourceData[frameCount] = {};
    for (uint32_t i = 0; i < frameCount; ++i) { sourceData[i] = 0.5f; }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.open_file(testFile.string(), numChannels, sampleRate));
        sink.start_recording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.close_file();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());

    AudioPlayerSource player;
    ASSERT_TRUE(player.load_file(testFile.string(), numChannels, sampleRate));
    EXPECT_TRUE(player.is_loaded());

    ASSERT_TRUE(player.load_from_memory(wavBytes.data(), wavBytes.size(), numChannels, sampleRate));
    EXPECT_TRUE(player.is_loaded());
    EXPECT_EQ(player.get_total_frames(), frameCount);

    player.unload_file();
    std::filesystem::remove(testFile);
}
