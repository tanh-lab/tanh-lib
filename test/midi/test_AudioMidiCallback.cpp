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
#include <memory>
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
    EXPECT_EQ(callback.midi_latency_ns(), k_block_ns);  // one block, nothing on top
}

// The claim of the class: a MIDI event shows up in the audio exactly one block after the
// moment it arrived — every event, to the sample.
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
        const auto at = static_cast<std::size_t>(frames[i] + k_block);
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
        const auto at = static_cast<std::size_t>(frames[i] + k_block);
        ASSERT_LT(at, rendered.size());
        EXPECT_EQ(rendered[at], static_cast<float>(40 + i)) << "note " << i << " at frame " << at;
    }
    EXPECT_TRUE(engine.m_in_order);
}

// Immediate placement: every event at the start of the first block after it arrived —
// never later than one block, and never earlier than it happened.
TEST(AudioMidiCallback, ImmediatePlacementPutsEventsAtBlockStarts) {
    ClickEngine engine;
    InputQueue queue(256);
    BlockEventCollectorConfig config;
    config.m_placement = EventPlacement::Immediate;
    AudioMidiCallback callback(engine, queue, 2, config);
    callback.prepare_to_play(48000, k_block);
    EXPECT_EQ(callback.midi_latency_ns(), 0);

    // The device, on time; each note is pushed when its moment has passed. The notes are
    // more than a block apart, so that each block start holds one click at most.
    std::vector<std::int64_t> frames;
    for (int i = 0; i < 40; ++i) {
        frames.push_back(static_cast<std::int64_t>(std::floor(1440.0 + i * 701.3)));
    }
    std::vector<float> rendered;
    std::vector<float> output(static_cast<std::size_t>(k_block) * 2);
    std::size_t next = 0;
    for (int block = 0; block < 70; ++block) {
        const std::int64_t now = k_t0 + block * k_block_ns;
        while (next < frames.size() && time_of_frame(static_cast<double>(frames[next])) <= now) {
            queue.push(make_midi1_channel_voice(0, 0x90, static_cast<std::uint8_t>(40 + next), 100),
                       time_of_frame(static_cast<double>(frames[next])),
                       0);
            ++next;
        }
        callback.process_at(now, output.data(), nullptr, k_block, 0, 2);
        for (std::uint32_t frame = 0; frame < k_block; ++frame) {
            rendered.push_back(output[frame * 2]);
        }
    }

    for (std::size_t i = 0; i < frames.size(); ++i) {
        // The first block boundary at or after the note's frame.
        const auto at = static_cast<std::size_t>((frames[i] / k_block + 1) * k_block);
        ASSERT_LT(at, rendered.size());
        EXPECT_EQ(rendered[at], static_cast<float>(40 + i)) << "note " << i;
        EXPECT_LE(static_cast<std::int64_t>(at) - frames[i], static_cast<std::int64_t>(k_block));
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

// Without an output there is nothing to render, and what is played meanwhile is not
// played late when the output is back.
TEST(AudioMidiCallback, WithoutOutputNothingRuns) {
    ClickEngine engine;
    InputQueue queue(64);
    AudioMidiCallback callback(engine, queue, 2);
    callback.prepare_to_play(48000, k_block);
    EXPECT_EQ(run(callback, 4).size(), 4U * k_block);

    queue.push(make_midi1_channel_voice(0, 0x90, 60, 100), k_t0 + 4 * k_block_ns, 0);
    callback.process_at(k_t0 + 5 * k_block_ns, nullptr, nullptr, k_block, 0, 0);
    EXPECT_EQ(engine.m_blocks, 4);

    // A second later the output is back: a new stream, without the old note.
    std::vector<float> output(static_cast<std::size_t>(k_block) * 2, -1.0f);
    callback.process_at(k_t0 + 1'000'000'000, output.data(), nullptr, k_block, 0, 2);
    EXPECT_EQ(engine.m_blocks, 5);
    for (const float sample : output) { EXPECT_EQ(sample, 0.0f); }
    TimedUmp packet;
    EXPECT_FALSE(queue.pop(packet));
}

// MIDI that piled up before the stream started is not fired at the engine in a burst.
TEST(AudioMidiCallback, MidiFromBeforeTheStreamIsNotPlayed) {
    ClickEngine engine;
    InputQueue queue(1024);
    AudioMidiCallback callback(engine, queue, 2);
    callback.prepare_to_play(48000, k_block);

    for (int i = 0; i < 600; ++i) {
        queue.push(make_midi1_channel_voice(0, 0x90, 60, 100), k_t0 - 5'000'000'000 + i * 1000, 0);
    }
    queue.push(make_midi1_channel_voice(0, 0x90, 72, 100), time_of_frame(100), 0);

    const std::vector<float> rendered = run(callback, 4);
    for (std::size_t i = 0; i < rendered.size(); ++i) {
        EXPECT_EQ(rendered[i], i == k_block + 100 ? 72.0f : 0.0f) << "sample " << i;
    }
    EXPECT_EQ(callback.dropped(), 0U);
}

// The engine gets the channels it was prepared for, also on a device with fewer.
TEST(AudioMidiCallback, EngineKeepsItsChannelsOnASmallerDevice) {
    class StereoEngine final : public AudioMidiProcessor {
    public:
        void prepare(double /*rate*/, std::size_t /*block*/, std::size_t /*channels*/) override {}
        void process(core::BufferView audio, std::span<const BlockEvent> /*events*/) override {
            m_channels = audio.get_num_channels();
            // What the second channel holds on entry, then a marker in each.
            m_second_on_entry = audio.get_write_pointer(1)[0];
            audio.get_write_pointer(0)[0] = 1.0f;
            audio.get_write_pointer(1)[0] = 2.0f;
        }
        std::size_t m_channels = 0;
        float m_second_on_entry = -1.0f;
    };

    StereoEngine engine;
    InputQueue queue(64);
    AudioMidiCallback callback(engine, queue, 2);
    callback.prepare_to_play(48000, k_block);

    std::vector<float> mono(k_block, -1.0f);
    callback.process_at(k_t0, mono.data(), nullptr, k_block, 0, 1);
    callback.process_at(k_t0 + k_block_ns, mono.data(), nullptr, k_block, 0, 1);

    EXPECT_EQ(engine.m_channels, 2U);
    EXPECT_EQ(engine.m_second_on_entry, 0.0f);  // cleared, not last block's marker
    EXPECT_EQ(mono[0], 1.0f);
    EXPECT_EQ(mono[1], 0.0f);
}

// process() is process_at() with the current time: what the audio device calls.
TEST(AudioMidiCallback, ProcessRendersABlockAtTheCurrentTime) {
    ClickEngine engine;
    InputQueue queue(64);
    AudioMidiCallback callback(engine, queue, 2);
    callback.prepare_to_play(48000, k_block);

    std::vector<float> output(static_cast<std::size_t>(k_block) * 2, -1.0f);
    callback.process(output.data(), nullptr, k_block, 0, 2);
    EXPECT_EQ(engine.m_blocks, 1);
    for (const float sample : output) { EXPECT_EQ(sample, 0.0f); }
}

// An output buffer without channels is no output either.
TEST(AudioMidiCallback, AnOutputWithoutChannelsIsNoOutput) {
    ClickEngine engine;
    InputQueue queue(64);
    AudioMidiCallback callback(engine, queue, 2);
    callback.prepare_to_play(48000, k_block);

    std::vector<float> output(static_cast<std::size_t>(k_block) * 2, -1.0f);
    callback.process_at(k_t0, output.data(), nullptr, k_block, 0, 0);
    EXPECT_EQ(engine.m_blocks, 0);
    for (const float sample : output) { EXPECT_EQ(sample, -1.0f); }
}

// An engine without state to clear need not override reset().
TEST(AudioMidiCallback, EngineNeedNotOverrideReset) {
    const std::unique_ptr<AudioMidiProcessor> engine = std::make_unique<ClickEngine>();
    engine->reset();
    engine->prepare(k_sample_rate, k_block, 2);
    EXPECT_EQ(static_cast<ClickEngine&>(*engine).m_blocks, 0);
}
