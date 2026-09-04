#pragma once

#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/SampleReader.h>

#include <algorithm>
#include <array>
#include <cstddef>

// The ChannelMode switch in its two deliberately different forms: a grain
// carries its own pan and Spread redistributes its energy across L/R; the
// single Sample head has nothing to pan, so Spread acts as mid/side width.
namespace thl::dsp::granular::channel_mixer {

using Frame = std::array<float, k_max_channel_support>;

// Average of all source channels at `position` (grain read: wrapped).
inline float mono_sum_wrapped(const SampleReader& reader,
                              float position,
                              size_t bank,
                              size_t source_channels) {
    float mono = 0.0f;
    for (size_t ch = 0; ch < source_channels; ++ch) {
        mono += reader.read_wrapped(position, bank, ch);
    }
    return source_channels > 1 ? mono / static_cast<float>(source_channels) : mono;
}
// Same for the head (clamped read, double position).
inline float mono_sum_clamped(const SampleReader& reader,
                              double position,
                              size_t bank,
                              size_t source_channels) {
    float mono = 0.0f;
    for (size_t ch = 0; ch < source_channels; ++ch) {
        mono += reader.read_clamped(position, bank, ch);
    }
    return source_channels > 1 ? mono / static_cast<float>(source_channels) : mono;
}

// Accumulate one grain sample (already windowed by `envelope`) into `accum`.
// `pan` is the grain's L/R position after Spread has been applied.
inline void accumulate_grain(const SampleReader& reader,
                             float position,
                             size_t bank,
                             size_t source_channels,
                             ChannelMode mode,
                             float pan,
                             float envelope,
                             size_t out_channels,
                             Frame& accum) {
    switch (mode) {
        case ChannelMode::MonoToStereo: {
            float const mono = mono_sum_wrapped(reader, position, bank, source_channels) * envelope;
            accum[0] += mono * (1.0f - pan);
            accum[1] += mono * pan;
            break;
        }
        case ChannelMode::TrueStereo: {
            float const s0 = reader.read_wrapped(position, bank, 0);
            float const s1 = source_channels > 1 ? reader.read_wrapped(position, bank, 1) : s0;
            // Spread redistributes per-grain energy across L/R
            accum[0] += s0 * envelope * (1.0f - pan) * k_stereo_energy_compensation;
            accum[1] += s1 * envelope * pan * k_stereo_energy_compensation;
            break;
        }
        case ChannelMode::TrueMultichannel: {
            size_t const channels = std::min(out_channels, source_channels);
            for (size_t ch = 0; ch < channels; ++ch) {
                float const s = reader.read_wrapped(position, bank, ch);
                // Even channels (0,2,...) get left energy, odd channels
                // (1,3,...) get right energy
                float const energy =
                    ((ch % 2 == 0) ? (1.0f - pan) : pan) * k_stereo_energy_compensation;
                accum[ch] += s * envelope * energy;
            }
            break;
        }
        default: break;
    }
}

// One frame of the Sample head at `position`; `width` is Spread as mid/side
// width (0 collapses to mono, 1 keeps the source's own image).
inline void read_head_frame(const SampleReader& reader,
                            double position,
                            size_t bank,
                            size_t source_channels,
                            ChannelMode mode,
                            float width,
                            size_t out_channels,
                            Frame& out) {
    out.fill(0.0f);
    switch (mode) {
        case ChannelMode::MonoToStereo: {
            // One head has no per-grain pan to spread, so the mono sum sits
            // centred; Spread has nothing to widen here.
            float const mono = mono_sum_clamped(reader, position, bank, source_channels);
            out[0] = mono;
            out[1] = mono;
            break;
        }
        case ChannelMode::TrueStereo: {
            float const s0 = reader.read_clamped(position, bank, 0);
            float const s1 = source_channels > 1 ? reader.read_clamped(position, bank, 1) : s0;
            float const mid = 0.5f * (s0 + s1);
            float const side = 0.5f * (s0 - s1) * width;
            out[0] = mid + side;
            out[1] = mid - side;
            break;
        }
        case ChannelMode::TrueMultichannel: {
            size_t const channels = std::min(out_channels, source_channels);
            for (size_t ch = 0; ch < channels; ++ch) {
                out[ch] = reader.read_clamped(position, bank, ch);
            }
            // Width per L/R pair (0,1), (2,3), ...
            for (size_t ch = 0; ch + 1 < channels; ch += 2) {
                float const mid = 0.5f * (out[ch] + out[ch + 1]);
                float const side = 0.5f * (out[ch] - out[ch + 1]) * width;
                out[ch] = mid + side;
                out[ch + 1] = mid - side;
            }
            break;
        }
        default: break;
    }
}

}  // namespace thl::dsp::granular::channel_mixer
