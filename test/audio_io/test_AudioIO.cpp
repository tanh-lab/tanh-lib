#include <gtest/gtest.h>
#include "tanh/audio_io/AudioDeviceInfo.h"
#include "tanh/audio_io/AudioDeviceManager.h"
#include "tanh/audio_io/AudioFileSink.h"
#include "tanh/audio_io/AudioIODeviceCallback.h"
#include "tanh/audio_io/AudioPlayerSource.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

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
    const ma_device_id* ptr = info.deviceIDPtr();
    EXPECT_EQ(ptr, &info.deviceID);
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
                 ma_uint32 frameCount,
                 ma_uint32 /*numInputChannels*/,
                 ma_uint32 numOutputChannels) override {
        ++processCallCount;
        // Fill output with silence
        if (!outputBuffer || numOutputChannels == 0) { return; }
        for (ma_uint32 i = 0; i < frameCount * numOutputChannels; ++i) {
            outputBuffer[i] = 0.0f;
        }
        (void)inputBuffer;
    }

    void prepareToPlay(ma_uint32 sampleRate, ma_uint32 bufferSize) override {
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

    constexpr ma_uint32 frameCount = 256;
    constexpr ma_uint32 numChannels = 2;
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
                 ma_uint32 frameCount,
                 ma_uint32 /*numInputChannels*/,
                 ma_uint32 numOutputChannels) override {
        ++processCallCount;
        if (!outputBuffer || numOutputChannels == 0) { return; }

        double phaseIncrement = (2.0 * M_PI * frequency) / sampleRate;

        for (ma_uint32 frame = 0; frame < frameCount; ++frame) {
            float sample = static_cast<float>(std::sin(phase) * 0.3);
            phase += phaseIncrement;
            if (phase >= 2.0 * M_PI) { phase -= 2.0 * M_PI; }

            for (ma_uint32 ch = 0; ch < numOutputChannels; ++ch) {
                outputBuffer[frame * numOutputChannels + ch] = sample;
            }
        }
    }

    void prepareToPlay(ma_uint32 rate, ma_uint32 /*bufferSize*/) override {
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

    constexpr ma_uint32 frameCount = 256;
    constexpr ma_uint32 numChannels = 2;
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

    constexpr ma_uint32 frameCount = 256;
    constexpr ma_uint32 numChannels = 2;
    float output[frameCount * numChannels] = {0};
    float input[frameCount * numChannels];

    for (ma_uint32 i = 0; i < frameCount * numChannels; ++i) {
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

    constexpr ma_uint32 frameCount = 256;
    constexpr ma_uint32 numChannels = 2;
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

// Integration test: record and play back
TEST(AudioFileIntegration, RecordAndPlayback) {
    std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    std::filesystem::path testFile = tempDir / "test_record_playback.wav";

    constexpr ma_uint32 frameCount = 1024;
    constexpr ma_uint32 numChannels = 2;
    constexpr ma_uint32 sampleRate = 48000;

    float originalData[frameCount * numChannels];
    for (ma_uint32 i = 0; i < frameCount * numChannels; ++i) {
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

        for (ma_uint32 i = 0; i < frameCount * numChannels; ++i) {
            EXPECT_FLOAT_EQ(playedData[i], originalData[i])
                << "Mismatch at sample " << i;
        }

        player.unloadFile();
        EXPECT_FALSE(player.isLoaded());
    }

    std::filesystem::remove(testFile);
}
