#pragma once

#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/sampler/SampleView.h>

#include <algorithm>
#include <array>
#include <cstddef>

// The ChannelMode switch in its two deliberately different forms: a grain
// carries its own pan and Spread redistributes its energy across L/R; a
// single play head has nothing to pan, so Spread acts as mid/side width.
namespace thl::dsp::granular::channel_mixer {

using Frame = std::array<float, k_max_channel_support>;

/// Average of all source channels at `position` (grain read: wrapped).
inline float mono_sum_wrapped(const sampler::SampleView& source, float position) {
    float mono = 0.0f;
    for (size_t ch = 0; ch < source.m_num_channels; ++ch) {
        mono += sampler::read_wrapped(source, ch, position);
    }
    return source.m_num_channels > 1 ? mono / static_cast<float>(source.m_num_channels) : mono;
}

/// Accumulate one grain sample (already windowed by `envelope`) into
/// `accum`. `pan` is the grain's L/R position after Spread has been applied.
inline void accumulate_grain(const sampler::SampleView& source,
                             float position,
                             ChannelMode mode,
                             float pan,
                             float envelope,
                             size_t out_channels,
                             Frame& accum) {
    switch (mode) {
        case ChannelMode::MonoToStereo: {
            float const mono = mono_sum_wrapped(source, position) * envelope;
            accum[0] += mono * (1.0f - pan);
            accum[1] += mono * pan;
            break;
        }
        case ChannelMode::TrueStereo: {
            float const s0 = sampler::read_wrapped(source, 0, position);
            float const s1 =
                source.m_num_channels > 1 ? sampler::read_wrapped(source, 1, position) : s0;
            // Spread redistributes per-grain energy across L/R
            accum[0] += s0 * envelope * (1.0f - pan) * k_stereo_energy_compensation;
            accum[1] += s1 * envelope * pan * k_stereo_energy_compensation;
            break;
        }
        case ChannelMode::TrueMultichannel: {
            size_t const channels = std::min(out_channels, source.m_num_channels);
            for (size_t ch = 0; ch < channels; ++ch) {
                float const s = sampler::read_wrapped(source, ch, position);
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

/**
 * @brief Map a play head's source channels to the output for a ChannelMode.
 *
 * `in` holds the head's render (source channel c in `in[c]`, a mono source
 * already duplicated); `source_channels` is the source's own channel count.
 * `width` is Spread as mid/side width (0 collapses to mono, 1 keeps the
 * source's image). Writes `num_frames` into the first `out_channels`
 * channels of `out` (overwriting); channels the mode does not feed are
 * zeroed. `in` and `out` must not alias.
 */
inline void mix_head(const float* const* in,
                     size_t source_channels,
                     ChannelMode mode,
                     float width,
                     float* const* out,
                     size_t out_channels,
                     size_t num_frames) {
    for (size_t ch = 0; ch < out_channels; ++ch) { std::fill(out[ch], out[ch] + num_frames, 0.0f); }
    switch (mode) {
        case ChannelMode::MonoToStereo: {
            // One head has no per-grain pan to spread, so the mono sum sits
            // centred. Half per side: what a centred grain gives under the
            // linear pan law above, so a mode switch does not step the level.
            for (size_t i = 0; i < num_frames; ++i) {
                float mono = 0.0f;
                for (size_t ch = 0; ch < source_channels; ++ch) { mono += in[ch][i]; }
                if (source_channels > 1) { mono /= static_cast<float>(source_channels); }
                mono *= 0.5f;
                if (out_channels > 0) { out[0][i] = mono; }
                if (out_channels > 1) { out[1][i] = mono; }
            }
            break;
        }
        case ChannelMode::TrueStereo: {
            for (size_t i = 0; i < num_frames; ++i) {
                float const s0 = in[0][i];
                float const s1 = source_channels > 1 ? in[1][i] : s0;
                float const mid = 0.5f * (s0 + s1);
                float const side = 0.5f * (s0 - s1) * width;
                if (out_channels > 0) { out[0][i] = mid + side; }
                if (out_channels > 1) { out[1][i] = mid - side; }
            }
            break;
        }
        case ChannelMode::TrueMultichannel: {
            size_t const channels = std::min(out_channels, source_channels);
            for (size_t i = 0; i < num_frames; ++i) {
                for (size_t ch = 0; ch < channels; ++ch) { out[ch][i] = in[ch][i]; }
                // Width per L/R pair (0,1), (2,3), ...
                for (size_t ch = 0; ch + 1 < channels; ch += 2) {
                    float const mid = 0.5f * (out[ch][i] + out[ch + 1][i]);
                    float const side = 0.5f * (out[ch][i] - out[ch + 1][i]) * width;
                    out[ch][i] = mid + side;
                    out[ch + 1][i] = mid - side;
                }
            }
            break;
        }
        default: break;
    }
}

}  // namespace thl::dsp::granular::channel_mixer
