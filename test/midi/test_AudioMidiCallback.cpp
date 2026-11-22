// AudioMidiCallback end to end, without devices: the test plays the audio device (it calls
// process_at() with the times a device would) and the MIDI inputs (it pushes timestamped
// packets). The engine marks every event in the audio it renders, so the output shows, to
// the sample, where each MIDI event landed.
#include <gtest/gtest.h>
#include <tanh/midi/AudioMidiCallback.h>
#include <tanh/midi/AudioMidiProcessor.h>
#include <tanh/midi/InputQueue.h>

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

using namespace thl;
using namespace thl::midi;

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr double k_ns_per_frame = 1.0e9 / k_sample_rate;
constexpr std::uint32_t k_block = 480;
constexpr std::int64_t k_block_ns = 10'000'000;
constexpr std::int64_t k_t0 = 7'000'000'000;
constexpr std::int64_t k_latency_frames = 48;  // the collector's default 1 ms

// An engine that passes its input through and adds a click of height (note number) at
// every note-on's sample offset in channel 0.
class ClickEngine final : public AudioMidiProcessor {
public:
    void prepare(double sample_rate,
                 std::size_t max_block_size,
                 std::size_t num_channels) override {
        m_sample_rate = sample_rate;
        m_max_block_size = max_block_size;
        m_num_channels = num_channels;
    }

    void process(core::BufferView audio, std::span<const BlockEvent> events) override {
        ++m_blocks;
        std::uint32_t previous = 0;
        for (const BlockEvent& event : events) {
            m_in_order = m_in_order && event.m_sample_offset >= previous &&
                         event.m_sample_offset < audio.get_num_samples();
            previous = event.m_sample_offset;
            audio.get_write_pointer(0)[event.m_sample_offset] +=
                static_cast<float>(event.m_midi.data1());
        }
    }

    double m_sample_rate = 0.0;
    std::size_t m_max_block_size = 0;
    std::size_t m_num_channels = 0;
    int m_blocks = 0;
    bool m_in_order = true;
};

std::int64_t time_of_frame(double frame) {
    return k_t0 + std::llround((frame + 0.5) * k_ns_per_frame);
}

// Runs `blocks` device callbacks of the given sizes (cycled), on time, and returns
// channel 0 of everything that was rendered.
std::vector<float> run(AudioMidiCallback& callback,
                       int blocks,
                       const std::vector<std::uint32_t>& sizes = {k_block}) {
    std::vector<float> rendered;
    std::vector<float> output;
    std::int64_t position = 0;
    for (int k = 0; k < blocks; ++k) {
        const std::uint32_t frames = sizes[static_cast<std::size_t>(k) % sizes.size()];
        output.assign(static_cast<std::size_t>(frames) * 2, -1.0f);
        const std::int64_t now =
            k_t0 + std::llround(static_cast<double>(position) * k_ns_per_frame);
        callback.process_at(now, output.data(), nullptr, frames, 0, 2);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            rendered.push_back(output[frame * 2]);
        }
        position += frames;
    }
    return rendered;
}

}  // namespace

TEST(AudioMidiCallback, PreparesTheEngine) {
    ClickEngine engine;
    InputQueue queue(64);
    AudioMidiCallback callback(engine, queue, 2);
    callback.prepare_to_play(48000, k_block);
    EXPECT_EQ(engine.m_sample_rate, 48000.0);
    EXPECT_EQ(engine.m_max_block_size, k_block);
    EXPECT_EQ(engine.m_num_channels, 2U);
    EXPECT_EQ(callback.midi_latency_ns(), k_block_ns + 1'000'000);
}

// The claim of the class: a MIDI event shows up in the audio exactly one block (plus the
// configured millisecond) after the moment it arrived — every event, to the sample.
TEST(AudioMidiCallback, MidiLandsInTheAudioAtItsSample) {
    ClickEngine engine;
    InputQueue queue(256);
    AudioMidiCallback callback(engine, queue, 2);
    callback.prepare_to_play(48000, k_block);

    // 40 notes, 7.37 ms apart, starting 30 ms into the stream.
    std::vector<std::int64_t> frames;
    for (int i = 0; i < 40; ++i) {
        frames.push_back(static_cast<std::int64_t>(std::floor(1440.0 + i * 353.7)));
        queue.push(make_midi1_channel_voice(0, 0x90, static_cast<std::uint8_t>(40 + i), 100),
                   time_of_frame(static_cast<double>(frames.back())),
                   0);
    }
    const std::vector<float> rendered = run(callback, 60);

    std::size_t clicks = 0;
    for (const float sample : rendered) { clicks += sample != 0.0f ? 1 : 0; }
    EXPECT_EQ(clicks, frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto at = static_cast<std::size_t>(frames[i] + k_block + k_latency_frames);
        ASSERT_LT(at, rendered.size());
        EXPECT_EQ(rendered[at], static_cast<float>(40 + i)) << "note " << i << " at frame " << at;
    }
    EXPECT_TRUE(engine.m_in_order);
    EXPECT_EQ(callback.dropped(), 0U);
}

// The same with the device handing over blocks of changing size: the place of an event in
// the audio does not depend on how the stream is cut into blocks.
TEST(AudioMidiCallback, AlignmentDoesNotDependOnBlockSizes) {
    ClickEngine engine;
    InputQueue queue(256);
    AudioMidiCallback callback(engine, queue, 2);
    callback.prepare_to_play(48000, k_block);

    std::vector<std::int64_t> frames;
    for (int i = 0; i < 40; ++i) {
        frames.push_back(static_cast<std::int64_t>(std::floor(1440.0 + i * 353.7)));
        queue.push(make_midi1_channel_voice(0, 0x90, static_cast<std::uint8_t>(40 + i), 100),
                   time_of_frame(static_cast<double>(frames.back())),
                   0);
    }
    const std::vector<float> rendered = run(callback, 200, {480, 64, 256, 480, 33, 128});

    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto at = static_cast<std::size_t>(frames[i] + k_block + k_latency_frames);
        ASSERT_LT(at, rendered.size());
        EXPECT_EQ(rendered[at], static_cast<float>(40 + i)) << "note " << i << " at frame " << at;
    }
    EXPECT_TRUE(engine.m_in_order);
}

// The engine works in place on the input; channels are converted both ways.
TEST(AudioMidiCallback, PassesAudioThroughInPlace) {
    ClickEngine engine;
    InputQueue queue(64);
    AudioMidiCallback callback(engine, queue, 2);
    callback.prepare_to_play(48000, k_block);

    // One input channel, four output channels, an engine with two.
    std::vector<float> input(k_block);
    for (std::uint32_t i = 0; i < k_block; ++i) { input[i] = 0.001f * static_cast<float>(i); }
    std::vector<float> output(static_cast<std::size_t>(k_block) * 4, -1.0f);
    callback.process_at(k_t0, output.data(), input.data(), k_block, 1, 4);

    for (std::uint32_t i = 0; i < k_block; ++i) {
        EXPECT_EQ(output[i * 4 + 0], input[i]);  // the input, passed through
        EXPECT_EQ(output[i * 4 + 1], 0.0f);      // engine channel without an input: silence
        EXPECT_EQ(output[i * 4 + 2], 0.0f);      // beyond the engine's channels: silence
        EXPECT_EQ(output[i * 4 + 3], 0.0f);
    }
    EXPECT_EQ(engine.m_blocks, 1);
}

// Without an output there is nothing to render; the MIDI is left alone.
TEST(AudioMidiCallback, WithoutOutputNothingRuns) {
    ClickEngine engine;
    InputQueue queue(64);
    AudioMidiCallback callback(engine, queue, 2);
    callback.prepare_to_play(48000, k_block);
    queue.push(make_midi1_channel_voice(0, 0x90, 60, 100), k_t0, 0);

    callback.process_at(k_t0, nullptr, nullptr, k_block, 0, 0);
    EXPECT_EQ(engine.m_blocks, 0);
    TimedUmp packet;
    EXPECT_TRUE(queue.pop(packet));
}
