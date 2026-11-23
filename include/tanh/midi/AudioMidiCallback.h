#pragma once

#include <tanh/audio-io/AudioIODeviceCallback.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Clock.h>
#include <tanh/midi/AudioMidiProcessor.h>
#include <tanh/midi/BlockEventCollector.h>
#include <tanh/midi/InputQueue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE {

/// Runs an AudioMidiProcessor from the hardware: the host of an app that has no host.
///
/// Register it as the audio callback (AudioDeviceManager::add_duplex_callback() or
/// add_playback_callback()) and give it the queue the MIDI inputs deliver into
/// (midi::DeviceManager::input_queue()). For every block the device asks for it
///
///  1. turns the MIDI that has arrived into the block's events, each at the sample offset
///     its arrival time maps to (midi::BlockEventCollector, midi::StreamClock),
///  2. hands the engine the block: the input audio in place, and those events,
///  3. gives the device what the engine rendered.
///
/// MIDI that was sent before the stream started, or while the device had no output, is
/// discarded rather than played late in one burst (midi::BlockEventCollector).
///
/// Audio and MIDI are aligned to the sample: two notes keep their distance in the audio
/// whatever the block size and however irregular the callbacks are, and every note is late
/// by the same amount, one full-size block (midi_latency_ns()) — the least that exact
/// placement allows, and what a plugin host gives a plugin. The way to less is a smaller
/// audio buffer; midi::EventPlacement::Immediate in the config trades the exact placement
/// for the shortest delay instead.
///
/// Header-only, because it joins two components: a target using it links tanh::AudioIO
/// and tanh::Midi.
class AudioMidiCallback final : public AudioIODeviceCallback {
public:
    /// `processor` and `queue` must outlive the callback's registration. `num_channels` is
    /// how many channels the engine gets, whatever the device has: output channels beyond
    /// that stay silent, and engine channels the device lacks are rendered and not played.
    AudioMidiCallback(AudioMidiProcessor& processor,
                      midi::InputQueue& queue,
                      std::size_t num_channels = 2,
                      const midi::BlockEventCollectorConfig& config = {})
        : m_processor(processor)
        , m_queue(queue)
        , m_collector(config)
        , m_num_channels(num_channels) {}

    void prepare_to_play(std::uint32_t sample_rate, std::uint32_t buffer_size) override {
        m_channels.assign(m_num_channels, std::vector<float>(buffer_size, 0.0f));
        m_channel_pointers.clear();
        for (auto& channel : m_channels) { m_channel_pointers.push_back(channel.data()); }
        m_collector.prepare(static_cast<double>(sample_rate), buffer_size);
        m_processor.prepare(static_cast<double>(sample_rate), buffer_size, m_num_channels);
    }

    void process(float* output_buffer,
                 const float* input_buffer,
                 std::uint32_t frame_count,
                 std::uint32_t num_input_channels,
                 std::uint32_t num_output_channels) TANH_NONBLOCKING_FUNCTION override {
        process_at(core::uptime_ns(),
                   output_buffer,
                   input_buffer,
                   frame_count,
                   num_input_channels,
                   num_output_channels);
    }

    /// process() with the time of the request given by the caller: for a host that has a
    /// better timestamp for the block than "now", and for tests. `now_ns` is on the clock
    /// of core::uptime_ns().
    void process_at(std::int64_t now_ns,
                    float* output_buffer,
                    const float* input_buffer,
                    std::uint32_t frame_count,
                    std::uint32_t num_input_channels,
                    std::uint32_t num_output_channels) TANH_NONBLOCKING_FUNCTION {
        // Without an output there is nothing to render into: the stream pauses for the
        // engine, and what is played meanwhile is not kept for when it resumes.
        if (output_buffer == nullptr || num_output_channels == 0) {
            m_collector.reset();
            return;
        }

        // The engine always gets the channels it was prepared for, and works in place:
        // its buffer starts out as the input, silent where the device has no input.
        for (std::size_t ch = 0; ch < m_num_channels; ++ch) {
            float* channel = m_channel_pointers[ch];
            if (input_buffer != nullptr && ch < num_input_channels) {
                for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
                    channel[frame] = input_buffer[frame * num_input_channels + ch];
                }
            } else {
                std::fill_n(channel, frame_count, 0.0f);
            }
        }

        const std::span<const BlockEvent> events =
            m_collector.collect(m_queue, now_ns, frame_count);
        m_processor.process(
            core::BufferView(m_channel_pointers.data(), m_num_channels, frame_count),
            events);

        // The device gets as many of the engine's channels as it has; the rest is silence.
        const std::size_t channels = std::min<std::size_t>(num_output_channels, m_num_channels);
        for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
            float* out = output_buffer + static_cast<std::size_t>(frame) * num_output_channels;
            for (std::size_t ch = 0; ch < channels; ++ch) {
                out[ch] = m_channel_pointers[ch][frame];
            }
            for (std::size_t ch = channels; ch < num_output_channels; ++ch) { out[ch] = 0.0f; }
        }
    }

    /// How late MIDI is rendered against its arrival: with exact placement one full-size
    /// block (plus the configured latency), with immediate placement 0 (see
    /// midi::BlockEventCollector::latency_ns()). Valid after prepare_to_play().
    [[nodiscard]] std::int64_t midi_latency_ns() const noexcept { return m_collector.latency_ns(); }

    /// Packets the collector had to drop because too many were waiting.
    [[nodiscard]] std::uint32_t dropped() const noexcept { return m_collector.dropped(); }

private:
    AudioMidiProcessor& m_processor;
    midi::InputQueue& m_queue;
    midi::BlockEventCollector m_collector;
    std::vector<std::vector<float>> m_channels;
    std::vector<float*> m_channel_pointers;
    std::size_t m_num_channels;
};

}  // namespace thl::inline THL_ABI_NAMESPACE
