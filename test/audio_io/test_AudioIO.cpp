#include <gtest/gtest.h>
#include "tanh/audio_io/AudioDeviceInfo.h"
#include "tanh/audio_io/AudioDeviceManager.h"
#include "tanh/audio_io/AudioFileLoader.h"
#include "tanh/audio_io/AudioFileSink.h"
#include "tanh/audio_io/AudioIODeviceCallback.h"
#include "tanh/audio_io/AudioPlayerSource.h"
#include "tanh/audio_io/DataSource.h"
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
    EXPECT_TRUE(info.name.empty());
    EXPECT_TRUE(info.sampleRates.empty());
}

TEST(AudioDeviceInfo, DeviceIDPointer) {
    AudioDeviceInfo info;
    const void* ptr = info.deviceIDPtr();
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
    bool initialised = manager.isContextInitialised();
    (void)initialised;  // Suppress unused warning
}

TEST(AudioDeviceManager, DefaultValues) {
    AudioDeviceManager manager;
    EXPECT_EQ(manager.getSampleRate(), 44100);
    EXPECT_EQ(manager.getBufferSize(), 512);
    EXPECT_EQ(manager.getNumOutputChannels(), 1);
    EXPECT_EQ(manager.getNumInputChannels(), 1);
    EXPECT_FALSE(manager.isPlaybackRunning());
}

TEST(AudioDeviceManager, EnumerateDevicesDoesNotCrash) {
    AudioDeviceManager manager;

    // These should not crash even if no devices are available
    auto inputDevices = manager.enumerateInputDevices();
    auto outputDevices = manager.enumerateOutputDevices();

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
    manager.stopPlayback();
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
        for (uint32_t i = 0; i < frameCount * numOutputChannels; ++i) {
            outputBuffer[i] = 0.0f;
        }
        (void)inputBuffer;
    }

    void prepareToPlay(uint32_t sampleRate, uint32_t bufferSize) override {
        ++prepareCallCount;
        (void)sampleRate;
        (void)bufferSize;
    }

    void releaseResources() override { ++releaseCallCount; }
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
    manager.addPlaybackCallback(&callback);
    manager.removePlaybackCallback(&callback);

    // Removing non-existent callback should be safe
    manager.removePlaybackCallback(&callback);
}

TEST(AudioDeviceManager, MultipleCallbacks) {
    AudioDeviceManager manager;
    MockCallback callback1;
    MockCallback callback2;
    MockCallback callback3;

    manager.addPlaybackCallback(&callback1);
    manager.addPlaybackCallback(&callback2);
    manager.addPlaybackCallback(&callback3);

    manager.removePlaybackCallback(&callback2);
    manager.removePlaybackCallback(&callback1);
    manager.removePlaybackCallback(&callback3);
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
                manager.addPlaybackCallback(&callbacks[idx]);
                addCount.fetch_add(1, std::memory_order_relaxed);

                std::this_thread::sleep_for(std::chrono::microseconds(50));

                manager.removePlaybackCallback(&callbacks[idx]);
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

    void prepareToPlay(uint32_t rate, uint32_t /*bufferSize*/) override {
        sampleRate = static_cast<double>(rate);
        phase = 0.0;
    }
};

TEST(HardwareTests, DISABLED_DeviceEnumeration) {
    AudioDeviceManager manager;

    if (!manager.isContextInitialised()) {
        GTEST_SKIP() << "Audio context not available";
    }

    auto inputs = manager.enumerateInputDevices();
    auto outputs = manager.enumerateOutputDevices();

    std::cout << "\n=== Input Devices (" << inputs.size() << ") ===\n";
    for (const auto& device : inputs) {
        std::cout << "  - " << device.name << " (rates: ";
        for (size_t i = 0; i < device.sampleRates.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << device.sampleRates[i];
        }
        std::cout << ")\n";
    }

    std::cout << "\n=== Output Devices (" << outputs.size() << ") ===\n";
    for (const auto& device : outputs) {
        std::cout << "  - " << device.name << " (rates: ";
        for (size_t i = 0; i < device.sampleRates.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << device.sampleRates[i];
        }
        std::cout << ")\n";
    }

    EXPECT_FALSE(outputs.empty()) << "No output devices found";
}

TEST(HardwareTests, DISABLED_PlaySineWave) {
    AudioDeviceManager manager;

    if (!manager.isContextInitialised()) {
        GTEST_SKIP() << "Audio context not available";
    }

    auto outputs = manager.enumerateOutputDevices();
    if (outputs.empty()) { GTEST_SKIP() << "No output devices available"; }

    std::cout << "\nUsing output device: " << outputs[0].name << "\n";

    SineCallback sineCallback(440.0);

    bool initialised =
        manager.initialise(nullptr, &outputs[0], 48000, 256, 0, 2);
    ASSERT_TRUE(initialised) << "Failed to initialise audio device";

    manager.addPlaybackCallback(&sineCallback);

    ASSERT_TRUE(manager.startPlayback()) << "Failed to start audio";
    EXPECT_TRUE(manager.isPlaybackRunning());

    std::cout << "Playing 440Hz sine wave for 500ms...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    manager.stopPlayback();
    EXPECT_FALSE(manager.isPlaybackRunning());

    std::cout << "Process callback was called " << sineCallback.processCallCount
              << " times\n";

    EXPECT_GT(sineCallback.processCallCount.load(), 0)
        << "Process callback was never called";
}

TEST(HardwareTests, DISABLED_StartStopCycles) {
    AudioDeviceManager manager;

    if (!manager.isContextInitialised()) {
        GTEST_SKIP() << "Audio context not available";
    }

    auto outputs = manager.enumerateOutputDevices();
    if (outputs.empty()) { GTEST_SKIP() << "No output devices available"; }

    SineCallback callback;

    ASSERT_TRUE(manager.initialise(nullptr, &outputs[0], 48000, 512, 0, 2));
    manager.addPlaybackCallback(&callback);

    for (int i = 0; i < 3; ++i) {
        std::cout << "Start/stop cycle " << (i + 1) << "/3\n";

        ASSERT_TRUE(manager.startPlayback());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        manager.stopPlayback();

        EXPECT_GT(callback.processCallCount.load(), 0);
    }

    std::cout << "Total process calls: " << callback.processCallCount << "\n";
}

TEST(HardwareTests, DISABLED_FullDuplex) {
    AudioDeviceManager manager;

    if (!manager.isContextInitialised()) {
        GTEST_SKIP() << "Audio context not available";
    }

    auto inputs = manager.enumerateInputDevices();
    auto outputs = manager.enumerateOutputDevices();

    if (inputs.empty() || outputs.empty()) {
        GTEST_SKIP() << "Need both input and output devices for duplex test";
    }

    std::cout << "\nInput: " << inputs[0].name << "\n";
    std::cout << "Output: " << outputs[0].name << "\n";

    MockCallback callback;

    bool initialised =
        manager.initialise(&inputs[0], &outputs[0], 48000, 512, 2, 2);
    ASSERT_TRUE(initialised) << "Failed to initialise duplex device";

    manager.addPlaybackCallback(&callback);

    ASSERT_TRUE(manager.startPlayback());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    manager.stopPlayback();

    std::cout << "Duplex process calls: " << callback.processCallCount << "\n";
    EXPECT_GT(callback.processCallCount, 0);
}

// =============================================================================
// AudioFileSink Tests
// =============================================================================

TEST(AudioFileSink, DefaultConstruction) {
    AudioFileSink sink;
    EXPECT_FALSE(sink.isOpen());
    EXPECT_FALSE(sink.isRecording());
    EXPECT_EQ(sink.getFramesWritten(), 0);
}

TEST(AudioFileSink, CloseWithoutOpen) {
    AudioFileSink sink;
    sink.closeFile();
    EXPECT_FALSE(sink.isOpen());
}

TEST(AudioFileSink, StartRecordingWithoutOpen) {
    AudioFileSink sink;
    sink.startRecording();
    EXPECT_FALSE(sink.isRecording());
}

TEST(AudioFileSink, StopRecordingWithoutStart) {
    AudioFileSink sink;
    sink.stopRecording();
    EXPECT_FALSE(sink.isRecording());
}

TEST(AudioFileSink, ProcessWithoutOpen) {
    AudioFileSink sink;

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 2;
    float output[frameCount * numChannels] = {0};
    float input[frameCount * numChannels] = {0};

    sink.process(output, input, frameCount, numChannels, 0);
    EXPECT_EQ(sink.getFramesWritten(), 0);
}

TEST(AudioFileSink, OpenAndCloseFile) {
    AudioFileSink sink;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_audio_sink.wav";

    bool opened = sink.openFile(testFile.string(), 2, 48000);
    EXPECT_TRUE(opened);
    EXPECT_TRUE(sink.isOpen());
    EXPECT_FALSE(sink.isRecording());

    sink.closeFile();
    EXPECT_FALSE(sink.isOpen());

    std::filesystem::remove(testFile);
}

TEST(AudioFileSink, RecordingStateToggle) {
    AudioFileSink sink;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_audio_sink_toggle.wav";

    ASSERT_TRUE(sink.openFile(testFile.string(), 2, 48000));

    EXPECT_FALSE(sink.isRecording());
    sink.startRecording();
    EXPECT_TRUE(sink.isRecording());
    sink.stopRecording();
    EXPECT_FALSE(sink.isRecording());

    sink.closeFile();
    std::filesystem::remove(testFile);
}

TEST(AudioFileSink, WriteFrames) {
    AudioFileSink sink;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_audio_sink_write.wav";

    ASSERT_TRUE(sink.openFile(testFile.string(), 2, 48000));
    sink.startRecording();

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 2;
    float output[frameCount * numChannels] = {0};
    float input[frameCount * numChannels];

    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        input[i] = static_cast<float>(i) /
                   static_cast<float>(frameCount * numChannels);
    }

    sink.process(output, input, frameCount, numChannels, 0);
    EXPECT_EQ(sink.getFramesWritten(), frameCount);

    sink.process(output, input, frameCount, numChannels, 0);
    EXPECT_EQ(sink.getFramesWritten(), frameCount * 2);

    sink.closeFile();
    EXPECT_TRUE(std::filesystem::exists(testFile));
    std::filesystem::remove(testFile);
}

TEST(AudioFileSink, RejectMismatchedInputChannels) {
    AudioFileSink sink;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile =
        tempDir / "test_audio_sink_channel_mismatch.wav";

    ASSERT_TRUE(sink.openFile(testFile.string(), 2, 48000));
    sink.startRecording();

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t inputChannels = 1;
    float output[frameCount * 2] = {0};
    float input[frameCount * inputChannels] = {0};

    sink.process(output, input, frameCount, inputChannels, 0);
    EXPECT_EQ(sink.getFramesWritten(), 0);

    sink.closeFile();
    std::filesystem::remove(testFile);
}

TEST(AudioFileSink, ReleaseResources) {
    AudioFileSink sink;

    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_audio_sink_release.wav";

    ASSERT_TRUE(sink.openFile(testFile.string(), 2, 48000));
    sink.startRecording();

    sink.releaseResources();
    EXPECT_FALSE(sink.isOpen());
    EXPECT_FALSE(sink.isRecording());

    std::filesystem::remove(testFile);
}

// =============================================================================
// AudioPlayerSource Tests
// =============================================================================

// =============================================================================
// AudioFileLoader Tests
// =============================================================================

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
    auto ds = loader.load_data_source_from_file("/nonexistent/path/audio.wav",
                                                48000, 2);
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
        ASSERT_TRUE(sink.openFile(testFile.string(), numChannels, sampleRate));
        sink.startRecording();
        float sinkOutput[frameCount * numChannels] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.closeFile();
    }

    thl::audio_io::AudioFileLoader loader;
    auto buffer =
        loader.load_from_file(testFile.string(), sampleRate, numChannels);
    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.get_num_channels(), numChannels);
    EXPECT_EQ(buffer.get_num_frames(), frameCount);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(),
                     static_cast<double>(sampleRate));

    for (uint32_t ch = 0; ch < numChannels; ++ch) {
        const float* ptr = buffer.get_read_pointer(ch);
        for (uint32_t f = 0; f < frameCount; ++f) {
            EXPECT_FLOAT_EQ(ptr[f],
                            sourceData[f * numChannels + ch])
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
        sourceData[i] = static_cast<float>(i) /
                        static_cast<float>(frameCount * numChannels);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.openFile(testFile.string(), numChannels, sampleRate));
        sink.startRecording();
        float sinkOutput[frameCount * numChannels] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.closeFile();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds =
        loader.load_data_source_from_file(testFile.string(), sampleRate,
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
        EXPECT_FLOAT_EQ(readBuf[i], sourceData[i])
            << "Mismatch at interleaved index " << i;
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
        ASSERT_TRUE(sink.openFile(testFile.string(), numChannels, sampleRate));
        sink.startRecording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.closeFile();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file(testFile.string(), sampleRate,
                                                numChannels);
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
        ASSERT_TRUE(sink.openFile(testFile.string(), numChannels, sampleRate));
        sink.startRecording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.closeFile();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file(testFile.string(), sampleRate,
                                                numChannels);
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
        ASSERT_TRUE(sink.openFile(testFile.string(), numChannels, sampleRate));
        sink.startRecording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.closeFile();
    }

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_file(testFile.string(), sampleRate,
                                                numChannels);
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
    EXPECT_FALSE(player.isLoaded());
    EXPECT_FALSE(player.isPlaying());
    EXPECT_EQ(player.getCurrentFrame(), 0);
    EXPECT_EQ(player.getTotalFrames(), 0);
}

TEST(AudioPlayerSource, UnloadWithoutLoad) {
    AudioPlayerSource player;
    player.unloadFile();
    EXPECT_FALSE(player.isLoaded());
}

TEST(AudioPlayerSource, PlayWithoutLoad) {
    AudioPlayerSource player;
    player.play();
    EXPECT_FALSE(player.isPlaying());
}

TEST(AudioPlayerSource, PauseWithoutLoad) {
    AudioPlayerSource player;
    player.pause();
    EXPECT_FALSE(player.isPlaying());
}

TEST(AudioPlayerSource, StopWithoutLoad) {
    AudioPlayerSource player;
    player.stop();
    EXPECT_FALSE(player.isPlaying());
}

TEST(AudioPlayerSource, SeekWithoutLoad) {
    AudioPlayerSource player;
    player.seekToFrame(100);
    EXPECT_EQ(player.getCurrentFrame(), 0);
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
    bool loaded = player.loadFile("/nonexistent/path/audio.wav", 2, 48000);
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(player.isLoaded());
}

TEST(AudioPlayerSource, SetFinishedCallback) {
    AudioPlayerSource player;
    bool callbackCalled = false;

    player.setFinishedCallback([&callbackCalled]() { callbackCalled = true; });

    EXPECT_FALSE(callbackCalled);
}

TEST(AudioPlayerSource, ReleaseResources) {
    AudioPlayerSource player;
    player.releaseResources();
    EXPECT_FALSE(player.isLoaded());
}

TEST(AudioPlayerSource, ProcessRejectsOutputChannelMismatch) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile =
        tempDir / "test_player_channel_mismatch.wav";

    constexpr uint32_t frameCount = 128;
    constexpr uint32_t writeChannels = 2;
    constexpr uint32_t sampleRate = 48000;
    float sourceData[frameCount * writeChannels] = {0.25f};

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.openFile(testFile.string(), writeChannels, sampleRate));
        sink.startRecording();
        float sinkOutput[frameCount * writeChannels] = {0};
        sink.process(sinkOutput, sourceData, frameCount, writeChannels, 0);
        sink.closeFile();
    }

    AudioPlayerSource player;
    ASSERT_TRUE(player.loadFile(testFile.string(), writeChannels, sampleRate));
    player.play();

    float output[frameCount] = {};
    for (auto& sample : output) { sample = 1.0f; }
    float input[frameCount] = {0};

    player.process(output, input, frameCount, 0, 1);

    for (const auto& sample : output) { EXPECT_FLOAT_EQ(sample, 1.0f); }

    player.unloadFile();
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
        ASSERT_TRUE(sink.openFile(testFile.string(), numChannels, sampleRate));
        sink.startRecording();

        float output[frameCount * numChannels] = {0};
        sink.process(output, originalData, frameCount, numChannels, 0);
        sink.closeFile();
    }

    ASSERT_TRUE(std::filesystem::exists(testFile));

    {
        AudioPlayerSource player;
        ASSERT_TRUE(
            player.loadFile(testFile.string(), numChannels, sampleRate));
        EXPECT_TRUE(player.isLoaded());
        EXPECT_EQ(player.getTotalFrames(), frameCount);
        EXPECT_EQ(player.getCurrentFrame(), 0);

        player.play();
        EXPECT_TRUE(player.isPlaying());

        float playedData[frameCount * numChannels] = {0};
        float input[frameCount * numChannels] = {0};

        player.process(playedData, input, frameCount, 0, numChannels);

        for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
            EXPECT_FLOAT_EQ(playedData[i], originalData[i])
                << "Mismatch at sample " << i;
        }

        player.unloadFile();
        EXPECT_FALSE(player.isLoaded());
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

TEST(AudioFileLoader, LoadDataSourceFromMemoryInvalidData) {
    thl::audio_io::AudioFileLoader loader;
    uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto ds = loader.load_data_source_from_memory(garbage, sizeof(garbage),
                                                   48000, 2);
    EXPECT_FALSE(ds.is_valid());
}

TEST(AudioFileLoader, LoadDataSourceFromMemoryRoundTrip) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile =
        tempDir / "test_ds_from_memory_roundtrip.wav";

    constexpr uint32_t frameCount = 256;
    constexpr uint32_t numChannels = 2;
    constexpr uint32_t sampleRate = 48000;

    float sourceData[frameCount * numChannels];
    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        sourceData[i] = static_cast<float>(i) /
                        static_cast<float>(frameCount * numChannels);
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.openFile(testFile.string(), numChannels, sampleRate));
        sink.startRecording();
        float sinkOutput[frameCount * numChannels] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.closeFile();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());
    std::filesystem::remove(testFile);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wavBytes.data(),
                                                   wavBytes.size(),
                                                   sampleRate, numChannels);
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
        EXPECT_FLOAT_EQ(readBuf[i], sourceData[i])
            << "Mismatch at interleaved index " << i;
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
        ASSERT_TRUE(sink.openFile(testFile.string(), numChannels, sampleRate));
        sink.startRecording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.closeFile();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());
    std::filesystem::remove(testFile);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wavBytes.data(),
                                                   wavBytes.size(),
                                                   sampleRate, numChannels);
    ASSERT_TRUE(ds.is_valid());

    EXPECT_TRUE(ds.seek(128));
    EXPECT_EQ(ds.get_cursor(), 128u);

    float readBuf[128] = {0};
    uint64_t framesRead = ds.read_pcm_frames(readBuf, 128);
    EXPECT_EQ(framesRead, 128u);

    for (uint32_t i = 0; i < 128; ++i) {
        EXPECT_FLOAT_EQ(readBuf[i], sourceData[128 + i])
            << "Mismatch at frame " << (128 + i);
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
        ASSERT_TRUE(sink.openFile(testFile.string(), numChannels, sampleRate));
        sink.startRecording();
        float sinkOutput[frameCount] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.closeFile();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());
    std::filesystem::remove(testFile);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wavBytes.data(),
                                                   wavBytes.size(),
                                                   sampleRate, numChannels);
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
    for (uint32_t i = 0; i < frameCount * numChannels; ++i) {
        sourceData[i] = 0.25f;
    }

    {
        AudioFileSink sink;
        ASSERT_TRUE(sink.openFile(testFile.string(), numChannels, sampleRate));
        sink.startRecording();
        float sinkOutput[frameCount * numChannels] = {0};
        sink.process(sinkOutput, sourceData, frameCount, numChannels, 0);
        sink.closeFile();
    }

    auto wavBytes = read_file_bytes(testFile);
    ASSERT_FALSE(wavBytes.empty());
    std::filesystem::remove(testFile);

    thl::audio_io::AudioFileLoader loader;
    auto ds = loader.load_data_source_from_memory(wavBytes.data(),
                                                   wavBytes.size());
    ASSERT_TRUE(ds.is_valid());
    EXPECT_EQ(ds.get_channel_count(), numChannels);
    EXPECT_EQ(ds.get_sample_rate(), sampleRate);
    EXPECT_EQ(ds.get_total_frames(), frameCount);
}
