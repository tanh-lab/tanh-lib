#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/midi/Ump.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE {

/// Something that happens at a sample position inside one audio block.
///
/// MIDI is the only kind today. The type tag is there so that further kinds — parameter
/// changes, transport — can join the same list later without changing
/// AudioMidiProcessor::process().
struct BlockEvent {
    enum class Type : std::uint8_t {
        Midi,  ///< m_midi holds one Universal MIDI Packet.
    };

    midi::Ump m_midi;                   ///< The packet, for Type::Midi.
    std::uint32_t m_sample_offset = 0;  ///< Position in the block, 0 to block size - 1.
    std::uint16_t m_source = 0;         ///< The input it came from (TimedUmp::m_source).
    Type m_type = Type::Midi;
};

/// The top of an audio engine as a host sees it: one call per block that gets the block's
/// audio and the events that fall inside it — the shape of a plugin's process callback
/// (JUCE's processBlock, CLAP's process), independent of who the host is.
///
/// A host is whatever owns the audio stream. In an app that is the audio device, through
/// AudioMidiCallback, which fills the events from the MIDI inputs; in a plugin it is a
/// wrapper that copies over what the DAW hands it. The engine behind the interface is the
/// same in both.
///
/// This is not the base of the DSP building blocks (dsp::BaseProcessor): those take audio
/// and modulation, and the engine decides what the events mean for them.
class TANH_API AudioMidiProcessor {
public:
    AudioMidiProcessor() = default;
    virtual ~AudioMidiProcessor() = default;

    AudioMidiProcessor(const AudioMidiProcessor&) = default;
    AudioMidiProcessor& operator=(const AudioMidiProcessor&) = default;
    AudioMidiProcessor(AudioMidiProcessor&&) = default;
    AudioMidiProcessor& operator=(AudioMidiProcessor&&) = default;

    /// Called before processing starts and whenever the stream changes, off the audio
    /// thread. No block will be larger than `max_block_size` frames.
    virtual void prepare(double sample_rate,
                         std::size_t max_block_size,
                         std::size_t num_channels) = 0;

    /// Renders one block in place: `audio` holds the input on entry (silence where there is
    /// none) and the output on return. `events` are the block's events in ascending
    /// m_sample_offset, each offset below audio.get_num_samples(). Audio thread: must not
    /// allocate or block.
    virtual void process(core::BufferView audio,
                         std::span<const BlockEvent> events) TANH_NONBLOCKING_FUNCTION = 0;

    /// Clears the engine's state (voices, delay lines) without changing its configuration.
    virtual void reset() TANH_NONBLOCKING_FUNCTION {}
};

}  // namespace thl::inline THL_ABI_NAMESPACE
