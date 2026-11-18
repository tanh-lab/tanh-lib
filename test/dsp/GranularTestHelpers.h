// Shared fixtures of the sampler / granular tests. Sources are RAMPS
// (x[n] = n): linear interpolation of a ramp is exact, so every output sample
// IS the head position and crossfade weights have closed forms.
#pragma once

#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/Numbers.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/ChannelMixer.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/VoiceParams.h>
#include <tanh/dsp/sampler/SamplePlayer.h>
#include <tanh/dsp/sampler/SampleView.h>
#include <tanh/dsp/slicing/SliceSteps.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace granular_test {

using namespace thl::dsp::granular;

inline constexpr double k_sample_rate = 48000.0;
// PlayerSettings::m_crossfade_seconds * k_sample_rate
inline constexpr size_t k_fade = 480;
inline constexpr size_t k_block = 64;

inline thl::core::BufferF make_ramp(size_t channels, size_t frames, float channel_offset = 0.0f) {
    thl::core::BufferF buffer(channels, frames, k_sample_rate);
    for (size_t ch = 0; ch < channels; ++ch) {
        float* p = buffer.get_write_pointer(ch);
        for (size_t i = 0; i < frames; ++i) {
            p[i] = static_cast<float>(i) + channel_offset * static_cast<float>(ch);
        }
    }
    return buffer;
}

inline void load(thl::dsp::audio::AudioDataStore& store, std::vector<thl::core::BufferF> banks) {
    auto& dst = store.begin_load();
    dst = std::move(banks);
    store.commit_load(0);
}

struct RecordingListener final : GrainVisualizationListener {
    struct Triggered {
        int m_slot;
        float m_pos;
        float m_len;
        float m_velocity;
        float m_ms;
    };

    // GrainVisualizer emits from inside the voice's render, i.e. the audio
    // thread — "the emitters are audio-thread safe", so a listener must not
    // allocate. Reserve once here on the setup thread and never let a
    // push_back reallocate: within capacity it cannot, and past capacity we
    // count the overflow instead. The budget is ~80x the busiest test in this
    // file (6016 frames at a 64-frame block = 94 head reports), so tripping
    // m_dropped means a new test outgrew it, not that the cap is too tight.
    static constexpr size_t k_capacity = 8192;

    std::vector<Triggered> m_triggered;
    std::vector<int> m_finished;
    size_t m_dropped{0};

    RecordingListener() {
        m_triggered.reserve(k_capacity);
        m_finished.reserve(k_capacity);
    }

    void on_grain_triggered(int slot, float pos, float len, float velocity, float ms) override {
        if (m_triggered.size() == m_triggered.capacity()) {
            ++m_dropped;
            return;
        }
        m_triggered.push_back({slot, pos, len, velocity, ms});
    }
    void on_grain_updated(int, float, float) override {}
    void on_grain_finished(int slot) override {
        if (m_finished.size() == m_finished.capacity()) {
            ++m_dropped;
            return;
        }
        m_finished.push_back(slot);
    }
    void on_master_envelope_updated(float) override {}
};

// Planar scratch with the pointer array render() calls take.
struct Block {
    std::vector<std::vector<float>> m_data;
    std::array<float*, thl::dsp::sampler::k_max_channels> m_ptrs{};
    size_t m_channels;
    size_t m_frames;
    Block(size_t channels, size_t frames)
        : m_data(channels, std::vector<float>(frames)), m_channels(channels), m_frames(frames) {
        for (size_t c = 0; c < channels; ++c) { m_ptrs[c] = m_data[c].data(); }
    }
    float* const* out() { return m_ptrs.data(); }
    void clear() {
        for (auto& c : m_data) { std::fill(c.begin(), c.end(), 0.0f); }
    }
};

// One view per bank of a store (what a host hands the components per block).
inline std::vector<thl::dsp::sampler::SampleView> views_of(
    const thl::dsp::audio::AudioDataStore& store) {
    std::vector<thl::dsp::sampler::SampleView> views;
    for (const auto& bank : store.get_buffer()) {
        views.push_back(thl::dsp::sampler::SampleView::of(bank));
    }
    return views;
}

// Equal-power crossfade weights at frame k of a k_fade-long fade.
inline float gain_in(size_t k) {
    return std::sin(static_cast<float>(k) / static_cast<float>(k_fade) * std::numbers::pi_v<float> *
                    0.5f);
}
inline float gain_out(size_t k) {
    return std::cos(static_cast<float>(k) / static_cast<float>(k_fade) * std::numbers::pi_v<float> *
                    0.5f);
}

// A SamplePlayer driven the way GrainProcessorImpl drives it: VoiceParams
// mapped to the player's controls, the channel mode applied after the render,
// the head reported to a GrainVisualizer. Sources are rebuilt per block, so a
// test may reload the store between renders.
struct PlayerRig {
    thl::dsp::audio::AudioDataStore m_store;
    GrainVisualizer m_viz;
    RecordingListener m_listener;
    thl::dsp::sampler::SamplePlayer m_player;
    VoiceParams m_params;
    std::vector<float> m_out;  // channel 0, everything rendered so far

    explicit PlayerRig(std::vector<thl::core::BufferF> banks) {
        load(m_store, std::move(banks));
        m_viz.add_listener(&m_listener);
        m_player.prepare(k_sample_rate);
        m_params.m_engine_mode = EngineMode::Sample;
        m_params.m_channel_mode = ChannelMode::TrueStereo;  // mono source -> out = source
        m_params.m_velocity = 1.0f;
        m_player.note_on();
    }

    // One block into `out` (overwritten).
    void render_block(float* const* out, size_t channels, size_t frames) {
        auto const views = views_of(m_store);
        size_t const source =
            views.empty()
                ? 0
                : static_cast<size_t>(
                      std::clamp(m_params.m_sample_index, 0, static_cast<int>(views.size()) - 1));
        size_t const source_channels = views.empty() ? 0 : views[source].m_num_channels;
        if (m_params.m_slicer && !views.empty()) {
            m_player.set_region(thl::dsp::slicing::region_from_steps(m_params.m_slices,
                                                                     m_params.m_sample_start,
                                                                     m_params.m_sample_end,
                                                                     views[source].m_num_frames));
        } else {
            m_player.set_markers(m_params.m_sample_start,
                                 m_params.m_sample_end,
                                 m_params.m_sample_loop_point);
        }
        m_player.set_speed(m_params.m_velocity);
        m_player.set_loop(m_params.m_loop);
        m_player.set_snap(m_params.m_loop_snap);

        Block head(k_max_channel_support, frames);
        size_t const head_channels = std::max<size_t>(2, source_channels);
        bool const was_started = m_player.started();
        bool const rendered = m_player.render(views, source, head.out(), head_channels, frames);
        m_viz.player_rendered(m_player, was_started, rendered, k_sample_rate);
        for (size_t ch = 0; ch < channels; ++ch) { std::fill_n(out[ch], frames, 0.0f); }
        if (rendered) {
            channel_mixer::mix_head(head.out(),
                                    source_channels,
                                    m_params.m_channel_mode,
                                    m_params.m_spread,
                                    out,
                                    channels,
                                    frames);
        }
    }

    // Render `frames` more (a multiple of k_block), appending channel 0.
    void render(size_t frames) {
        Block block(2, k_block);
        for (size_t done = 0; done < frames; done += k_block) {
            render_block(block.out(), 2, k_block);
            m_out.insert(m_out.end(), block.m_data[0].begin(), block.m_data[0].end());
        }
    }
};

}  // namespace granular_test
