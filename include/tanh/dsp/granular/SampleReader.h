#pragma once

#include <tanh/core/Buffer.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/GranularTypes.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace thl::dsp::granular {

// Linear interpolation at `position` in a channel of `frames` samples;
// positions past the end wrap around (a grain may be scheduled to
// overshoot). The one implementation behind every grain read.
inline float interpolate_wrapped(const float* data, size_t frames, float position) {
    auto pos_floor = static_cast<FramePos>(position);
    FramePos pos_ceil = pos_floor + 1;
    float const frac = position - static_cast<float>(pos_floor);
    auto const wrap = static_cast<FramePos>(frames);
    while (pos_ceil >= wrap) { pos_ceil -= wrap; }
    while (pos_floor >= wrap) { pos_floor -= wrap; }
    return data[pos_floor] * (1.0f - frac) + data[pos_ceil] * frac;
}

// A bank resolved once per block for the render loop: raw channel
// pointers, no per-sample validation. Invalid or empty banks give
// m_frames == 0 and must not be read.
struct BankView {
    std::array<const float*, k_max_channel_support> m_channels{};
    size_t m_frames{0};
    size_t m_num_channels{0};
    bool valid() const { return m_frames > 0; }
};

// Read-only view over the voice's source material: the pitch banks in an
// AudioDataStore. Stateless; every read is bounds-checked and interpolated.
// Precondition: the host holds the store's read state around every call
// (AudioDataStore::begin_read / end_read) — the loader replaces the banks
// outside of it.
class SampleReader {
public:
    explicit SampleReader(const audio::AudioDataStore& store) : m_store(store) {}

    bool is_loaded() const { return m_store.is_loaded(); }
    const std::vector<core::BufferF>& banks() const { return m_store.get_buffer(); }
    size_t num_banks() const { return banks().size(); }

    // A bank exists and holds audio. Unselected semitones are empty buffers
    // and must be treated as silence, never read.
    bool bank_valid(size_t bank) const {
        const auto& b = banks();
        return bank < b.size() && !b[bank].empty();
    }
    size_t num_frames(size_t bank) const {
        return bank_valid(bank) ? banks()[bank].get_num_samples() : 0;
    }
    size_t num_channels(size_t bank) const {
        return bank_valid(bank) ? banks()[bank].get_num_channels() : 0;
    }
    // Clamp a raw bank index into range. Requires num_banks() > 0.
    size_t clamp_bank(int raw) const {
        return static_cast<size_t>(std::clamp(raw, 0, static_cast<int>(num_banks()) - 1));
    }

    // Grain read (bounds-checked): see interpolate_wrapped.
    float read_wrapped(float position, size_t bank, size_t channel) const {
        if (!bank_valid(bank)) { return 0.0f; }
        const auto& buf = banks()[bank];
        if (channel >= buf.get_num_channels()) { return 0.0f; }
        return interpolate_wrapped(buf.get_read_pointer(channel), buf.get_num_samples(), position);
    }

    BankView view(size_t bank) const {
        BankView v;
        if (!bank_valid(bank)) { return v; }
        const auto& buf = banks()[bank];
        v.m_frames = buf.get_num_samples();
        v.m_num_channels = std::min(buf.get_num_channels(), k_max_channel_support);
        for (size_t ch = 0; ch < v.m_num_channels; ++ch) {
            v.m_channels[ch] = buf.get_read_pointer(ch);
        }
        return v;
    }

    // Head read: full double-precision addressing (a float head loses
    // sample-exact positions past ~2^24 frames) and clamped to the last
    // frame instead of wrapping — an outgoing crossfade head runs past the
    // region end and must hold the last frame, never fold back onto the
    // material the incoming head is playing.
    float read_clamped(double position, size_t bank, size_t channel) const {
        if (!bank_valid(bank)) { return 0.0f; }
        const auto& buf = banks()[bank];
        if (channel >= buf.get_num_channels()) { return 0.0f; }
        size_t const num_frames = buf.get_num_samples();
        if (num_frames == 0) { return 0.0f; }

        double const pos = std::clamp(position, 0.0, static_cast<double>(num_frames - 1));
        auto const frame_a = static_cast<size_t>(pos);
        auto const frame_b = std::min(frame_a + 1, num_frames - 1);
        float const frac = static_cast<float>(pos - static_cast<double>(frame_a));
        const float* data = buf.get_read_pointer(channel);
        return data[frame_a] * (1.0f - frac) + data[frame_b] * frac;
    }

private:
    const audio::AudioDataStore& m_store;
};

}  // namespace thl::dsp::granular
