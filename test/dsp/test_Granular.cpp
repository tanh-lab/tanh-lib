// Granular voice components driven directly — no host, no modulation
// matrix. Sources are RAMPS (x[n] = n): linear interpolation of a ramp is
// exact, so every output sample IS the head position and crossfade weights
// have closed forms. That replaces "no step bigger than 5x the slope"
// bounds with arithmetic.

#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/Numbers.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/GrainEngine.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/HeadPolicy.h>
#include <tanh/dsp/granular/SamplePlayer.h>
#include <tanh/dsp/granular/SampleReader.h>
#include <tanh/dsp/granular/SampleRegion.h>
#include <tanh/dsp/granular/VoiceParams.h>

#include <cmath>
#include <random>
#include <utility>
#include <vector>

using namespace thl::dsp::granular;

namespace {

constexpr double k_sample_rate = 48000.0;
// k_player_crossfade_duration * k_sample_rate
constexpr size_t k_fade = 480;
constexpr size_t k_block = 64;

thl::core::BufferF make_ramp(size_t channels, size_t frames, float channel_offset = 0.0f) {
    thl::core::BufferF buffer(channels, frames, k_sample_rate);
    for (size_t ch = 0; ch < channels; ++ch) {
        float* p = buffer.get_write_pointer(ch);
        for (size_t i = 0; i < frames; ++i) {
            p[i] = static_cast<float>(i) + channel_offset * static_cast<float>(ch);
        }
    }
    return buffer;
}

void load(thl::dsp::audio::AudioDataStore& store, std::vector<thl::core::BufferF> banks) {
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
    std::vector<Triggered> m_triggered;
    std::vector<int> m_finished;

    void on_grain_triggered(int slot, float pos, float len, float velocity, float ms) override {
        m_triggered.push_back({slot, pos, len, velocity, ms});
    }
    void on_grain_updated(int, float, float) override {}
    void on_grain_finished(int slot) override { m_finished.push_back(slot); }
    void on_master_envelope_updated(float) override {}
};

// Planar scratch an AudioBlock points into.
struct Block {
    std::vector<std::vector<float>> m_data;
    AudioBlock m_view;
    Block(size_t channels, size_t frames) : m_data(channels, std::vector<float>(frames)) {
        m_view.m_num_channels = channels;
        m_view.m_num_frames = frames;
        for (size_t c = 0; c < channels; ++c) { m_view.m_channels[c] = m_data[c].data(); }
    }
    void clear() {
        for (auto& c : m_data) { std::fill(c.begin(), c.end(), 0.0f); }
    }
};

// Equal-power crossfade weights at frame k of a k_fade-long fade.
float gain_in(size_t k) {
    return std::sin(static_cast<float>(k) / static_cast<float>(k_fade) * std::numbers::pi_v<float> *
                    0.5f);
}
float gain_out(size_t k) {
    return std::cos(static_cast<float>(k) / static_cast<float>(k_fade) * std::numbers::pi_v<float> *
                    0.5f);
}

struct PlayerRig {
    thl::dsp::audio::AudioDataStore m_store;
    SampleReader m_reader{m_store};
    GrainVisualizer m_viz;
    RecordingListener m_listener;
    SamplePlayer m_player{m_reader, m_viz};
    VoiceParams m_params;
    std::vector<float> m_out;  // channel 0, everything rendered so far

    explicit PlayerRig(std::vector<thl::core::BufferF> banks) {
        load(m_store, std::move(banks));
        m_viz.add_listener(&m_listener);
        m_player.prepare(k_sample_rate, 2);
        m_params.m_engine_mode = EngineMode::Sample;
        m_params.m_channel_mode = ChannelMode::TrueStereo;  // mono source -> out = source
        m_params.m_velocity = 1.0f;
        m_player.note_on();
    }

    // Render `frames` more (a multiple of k_block), appending channel 0.
    void render(size_t frames) {
        Block block(2, k_block);
        for (size_t done = 0; done < frames; done += k_block) {
            block.clear();
            m_player.render(block.m_view, m_params);
            m_out.insert(m_out.end(), block.m_data[0].begin(), block.m_data[0].end());
        }
    }
};

}  // namespace

// ── SamplePlayer ─────────────────────────────────────────────────────────────

TEST(Granular, SamplePlayerLoopWrapIsExactEqualPowerCrossfade) {
    // Region [0, 4800), Loop at 1000: the head reaches End after 4800 frames,
    // then the tail rides on past End while the live head fades in at Loop.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_end = 0.48f;
    rig.m_params.m_sample_loop_point = 0.1f;
    rig.render(6016);

    for (size_t n = 0; n < 4800; ++n) { ASSERT_NEAR(rig.m_out[n], static_cast<float>(n), 1e-3f); }
    for (size_t k = 0; k < k_fade; ++k) {
        float const expected =
            gain_in(k) * static_cast<float>(1000 + k) + gain_out(k) * static_cast<float>(4800 + k);
        ASSERT_NEAR(rig.m_out[4800 + k], expected, 0.05f) << "k=" << k;
    }
    // After the fade only the live head remains, continuing from Loop.
    for (size_t k = k_fade; k < 1200; ++k) {
        ASSERT_NEAR(rig.m_out[4800 + k], static_cast<float>(1000 + k), 1e-3f) << "k=" << k;
    }
}

TEST(Granular, SamplePlayerRetriggerInsideFadeParksLiveHeadWithItsGain) {
    // Same wrap, then a note-on 256 frames into the 480-frame wrap fade. The
    // live head (fading in, gain sin) is parked as a second tail with that
    // gain; the first tail keeps fading; the live head restarts at Start.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_end = 0.48f;
    rig.m_params.m_sample_loop_point = 0.1f;
    rig.render(4800 + 256);
    rig.m_player.note_on();
    rig.render(1024);

    constexpr size_t k_retrigger = 4800 + 256;
    float const parked_gain = gain_in(256);
    for (size_t j = 0; j < k_fade; ++j) {
        float expected = gain_in(j) * static_cast<float>(j) +
                         parked_gain * gain_out(j) * static_cast<float>(1256 + j);
        if (256 + j < k_fade) { expected += gain_out(256 + j) * static_cast<float>(5056 + j); }
        ASSERT_NEAR(rig.m_out[k_retrigger + j], expected, 0.05f) << "j=" << j;
    }
    ASSERT_NEAR(rig.m_out[k_retrigger + k_fade], static_cast<float>(k_fade), 1e-3f);
}

TEST(Granular, SamplePlayerBankSwitchCrossfadesAndKeepsHeadPosition) {
    // Bank 1 is bank 0 + 1000. Switching mid-play keeps the head position:
    // the old bank fades out at the head, the new one fades in at the head.
    std::vector<thl::core::BufferF> banks;
    banks.push_back(make_ramp(1, 10000));
    thl::core::BufferF plus(1, 10000, k_sample_rate);
    float* d = plus.get_write_pointer(0);
    for (size_t i = 0; i < 10000; ++i) { d[i] = static_cast<float>(i) + 1000.0f; }
    banks.push_back(std::move(plus));
    PlayerRig rig(std::move(banks));
    rig.render(2048);
    rig.m_params.m_sample_index = 1;
    rig.render(1024);

    for (size_t k = 0; k < k_fade; ++k) {
        float const expected =
            gain_out(k) * static_cast<float>(2048 + k) + gain_in(k) * static_cast<float>(3048 + k);
        ASSERT_NEAR(rig.m_out[2048 + k], expected, 0.05f) << "k=" << k;
    }
    for (size_t k = k_fade; k < 1024; ++k) {
        ASSERT_NEAR(rig.m_out[2048 + k], static_cast<float>(3048 + k), 1e-3f) << "k=" << k;
    }
}

// ── Head policies ────────────────────────────────────────────────────────────

TEST(Granular, LoopScanHeadResumesScanWhenRegionShrinksBelowHead) {
    // Scan to 48000, then End drops to 20000 with temperature 0 (the
    // default). The next grain lands on Loop — and the scan must go on from
    // there, not return Loop for every grain until the next note-on.
    LoopScanHead head;
    std::mt19937 rng(1);
    VoiceParams params;
    constexpr size_t k_interval = 9600;

    auto const full = SampleRegion::full(96000);
    for (int i = 0; i < 5; ++i) { head.pick_start(full, 0.0f, k_interval, params, rng); }

    SampleRegion const small{.m_start = 0, .m_end = 20000, .m_loop_point = 0};
    FramePos const first = head.pick_start(small, 0.0f, k_interval, params, rng);
    FramePos const second = head.pick_start(small, 0.0f, k_interval, params, rng);
    FramePos const third = head.pick_start(small, 0.0f, k_interval, params, rng);
    EXPECT_EQ(first, FramePos{0});
    EXPECT_EQ(second, static_cast<FramePos>(k_interval));
    EXPECT_EQ(third, static_cast<FramePos>(2 * k_interval));
}

TEST(Granular, PositionSprayHeadTiltClipsTheWindow) {
    PositionSprayHead head;
    std::mt19937 rng(7);
    VoiceParams params;
    params.m_position = 0.5f;
    // 0.4 keeps the tilted window inside the sample. A window past either
    // edge currently wraps to the other end (jitter_and_wrap); whether that
    // should clip instead is an open product decision.
    params.m_spray = 0.4f;
    auto const region = SampleRegion::full(96000);
    auto const centre = static_cast<FramePos>(0.5f * static_cast<float>(96000 - 1));

    params.m_tilt = 1.0f;
    for (int i = 0; i < 500; ++i) {
        EXPECT_GE(head.pick_start(region, 0.0f, 0, params, rng), centre);
    }
    params.m_tilt = -1.0f;
    for (int i = 0; i < 500; ++i) {
        EXPECT_LE(head.pick_start(region, 0.0f, 0, params, rng), centre);
    }
    params.m_spray = 0.0f;
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(head.pick_start(region, 0.0f, 0, params, rng), centre);
    }
}

// ── GrainEngine ──────────────────────────────────────────────────────────────

TEST(Granular, GrainEngineLoopScanIsOneXRegardlessOfDensity) {
    // With no jitter every grain must start exactly where the scan head is,
    // and the head advances one trigger interval per trigger: density and
    // advance cancel, so grain k starts at the frame it was triggered on.
    for (float density : {0.0f, 1.0f}) {
        thl::dsp::audio::AudioDataStore store;
        load(store, [] {
            std::vector<thl::core::BufferF> b;
            b.push_back(make_ramp(1, 96000));
            return b;
        }());
        SampleReader reader(store);
        GrainVisualizer viz;
        RecordingListener listener;
        viz.add_listener(&listener);
        GrainEngine engine(reader, viz);
        engine.prepare(k_sample_rate, 2);
        engine.seed(3);
        engine.reset_schedule(EngineMode::GranularLoop);

        VoiceParams params;
        params.m_channel_mode = ChannelMode::TrueStereo;
        params.m_density = density;
        params.m_size = 0.0f;  // 2 ms grains, so the pool never fills

        Block block(2, k_block);
        size_t elapsed = 0;
        while (elapsed < 72000) {
            block.clear();
            engine.render(block.m_view, params, EngineMode::GranularLoop, elapsed);
            elapsed += k_block;
        }

        ASSERT_GE(listener.m_triggered.size(), 3u) << "density " << density;
        float const rate =
            k_min_grain_rate * std::pow(k_max_grain_rate / k_min_grain_rate, density);
        auto const interval = static_cast<size_t>(k_sample_rate / rate);
        for (size_t k = 0; k < listener.m_triggered.size(); ++k) {
            auto const start =
                static_cast<size_t>(std::lround(listener.m_triggered[k].m_pos * 96000.0f));
            EXPECT_EQ(start, k * interval) << "density " << density << " grain " << k;
        }
    }
}

// ── Region and reader edges ──────────────────────────────────────────────────

TEST(Granular, SampleRegionFromNormalizedClampsEveryEdge) {
    auto r = SampleRegion::from_normalized(0.5f, 0.25f, 0.0f, 1000);  // end < start
    EXPECT_EQ(r.size(), 0u);
    r = SampleRegion::from_normalized(0.0f, 0.5f, 0.9f, 1000);  // loop > end
    EXPECT_EQ(r.m_loop_point, 500u);
    r = SampleRegion::from_normalized(-2.0f, 3.0f, -1.0f, 1000);  // out of range
    EXPECT_EQ(r.m_start, 0u);
    EXPECT_EQ(r.m_end, 1000u);
    EXPECT_EQ(r.m_loop_point, 0u);
    r = SampleRegion::from_normalized(1.0f, 1.0f, 1.0f, 1000);  // empty at the end
    EXPECT_EQ(r.size(), 0u);
}

TEST(Granular, SampleReaderWrapsGrainsAndClampsHeads) {
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        b.push_back(make_ramp(2, 100, 1000.0f));
        b.emplace_back();  // unselected semitone: empty bank
        return b;
    }());
    SampleReader reader(store);

    EXPECT_TRUE(reader.bank_valid(0));
    EXPECT_FALSE(reader.bank_valid(1));
    EXPECT_EQ(reader.num_frames(1), 0u);
    EXPECT_FLOAT_EQ(reader.read_wrapped(99.5f, 0, 0), 0.5f * 99.0f + 0.5f * 0.0f);  // wraps to 0
    EXPECT_FLOAT_EQ(reader.read_clamped(99.5, 0, 0), 99.0f);                        // holds last
    EXPECT_FLOAT_EQ(reader.read_clamped(5000.0, 0, 0), 99.0f);
    EXPECT_FLOAT_EQ(reader.read_clamped(10.25, 0, 1), 1010.25f);  // channel 1 = ramp + 1000
    EXPECT_FLOAT_EQ(reader.read_wrapped(10.0f, 0, 5), 0.0f);      // no such channel
    EXPECT_FLOAT_EQ(reader.read_clamped(10.0, 1, 0), 0.0f);       // empty bank
}
