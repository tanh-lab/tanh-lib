// Granular voice components driven directly — no host, no modulation
// matrix. Sources are RAMPS (x[n] = n): linear interpolation of a ramp is
// exact, so every output sample IS the head position and crossfade weights
// have closed forms. That replaces "no step bigger than 5x the slope"
// bounds with arithmetic.

#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Numbers.h>
#include <tanh/dsp/audio/AudioDataStore.h>
#include <tanh/dsp/granular/GrainEngine.h>
#include <tanh/dsp/granular/GrainProcessor.h>
#include <tanh/dsp/granular/GrainVisualizer.h>
#include <tanh/dsp/granular/GranularTypes.h>
#include <tanh/dsp/granular/HeadPolicy.h>
#include <tanh/dsp/granular/SamplePlayer.h>
#include <tanh/dsp/granular/SampleReader.h>
#include <tanh/dsp/granular/SampleRegion.h>
#include <tanh/dsp/granular/VoiceParams.h>

#include <algorithm>
#include <array>
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

// ── Channel mixer ────────────────────────────────────────────────────────────

TEST(Granular, ChannelMixerModeMatrixIsExactAndHeadMatchesCentredGrain) {
    // Stereo source: channel 1 = channel 0 + 1000. At position 10 the
    // samples are 10 and 1010, mono average 510.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        b.push_back(make_ramp(2, 100, 1000.0f));
        b.push_back(make_ramp(1, 100));
        b.push_back(make_ramp(4, 100, 1000.0f));
        return b;
    }());
    SampleReader reader(store);
    using channel_mixer::Frame;
    auto grain = [&](size_t bank, size_t src_ch, ChannelMode mode, float pan, size_t out_ch) {
        Frame f{};
        channel_mixer::accumulate_grain(reader, 10.0f, bank, src_ch, mode, pan, 1.0f, out_ch, f);
        return f;
    };
    auto head = [&](size_t bank, size_t src_ch, ChannelMode mode, float width, size_t out_ch) {
        Frame f{};
        channel_mixer::read_head_frame(reader, 10.0, bank, src_ch, mode, width, out_ch, f);
        return f;
    };

    // MonoToStereo: centred grain and head agree (half per side).
    auto g = grain(0, 2, ChannelMode::MonoToStereo, 0.5f, 2);
    auto h = head(0, 2, ChannelMode::MonoToStereo, 0.0f, 2);
    EXPECT_FLOAT_EQ(g[0], 255.0f);
    EXPECT_FLOAT_EQ(g[1], 255.0f);
    EXPECT_FLOAT_EQ(h[0], g[0]);
    EXPECT_FLOAT_EQ(h[1], g[1]);
    // Hard-left grain keeps the mono sum.
    g = grain(0, 2, ChannelMode::MonoToStereo, 0.0f, 2);
    EXPECT_FLOAT_EQ(g[0] + g[1], 510.0f);

    // TrueStereo: centred grain = source (2x compensation); head width 1 =
    // source, width 0 = mid in both.
    g = grain(0, 2, ChannelMode::TrueStereo, 0.5f, 2);
    EXPECT_FLOAT_EQ(g[0], 10.0f);
    EXPECT_FLOAT_EQ(g[1], 1010.0f);
    h = head(0, 2, ChannelMode::TrueStereo, 1.0f, 2);
    EXPECT_FLOAT_EQ(h[0], 10.0f);
    EXPECT_FLOAT_EQ(h[1], 1010.0f);
    h = head(0, 2, ChannelMode::TrueStereo, 0.0f, 2);
    EXPECT_FLOAT_EQ(h[0], 510.0f);
    EXPECT_FLOAT_EQ(h[1], 510.0f);
    // Mono source is duplicated.
    g = grain(1, 1, ChannelMode::TrueStereo, 0.5f, 2);
    EXPECT_FLOAT_EQ(g[0], 10.0f);
    EXPECT_FLOAT_EQ(g[1], 10.0f);
    h = head(1, 1, ChannelMode::TrueStereo, 1.0f, 2);
    EXPECT_FLOAT_EQ(h[0], 10.0f);
    EXPECT_FLOAT_EQ(h[1], 10.0f);

    // TrueMultichannel: even channels take left energy, odd right; width
    // per pair; output truncated to the channels the voice has.
    g = grain(2, 4, ChannelMode::TrueMultichannel, 0.25f, 4);
    EXPECT_FLOAT_EQ(g[0], 10.0f * 0.75f * 2.0f);
    EXPECT_FLOAT_EQ(g[1], 1010.0f * 0.25f * 2.0f);
    EXPECT_FLOAT_EQ(g[2], 2010.0f * 0.75f * 2.0f);
    EXPECT_FLOAT_EQ(g[3], 3010.0f * 0.25f * 2.0f);
    h = head(2, 4, ChannelMode::TrueMultichannel, 1.0f, 4);
    EXPECT_FLOAT_EQ(h[2], 2010.0f);
    EXPECT_FLOAT_EQ(h[3], 3010.0f);
    h = head(2, 4, ChannelMode::TrueMultichannel, 0.0f, 4);
    EXPECT_FLOAT_EQ(h[2], 2510.0f);
    EXPECT_FLOAT_EQ(h[3], 2510.0f);
    g = grain(2, 4, ChannelMode::TrueMultichannel, 0.5f, 2);
    EXPECT_FLOAT_EQ(g[2], 0.0f);
    EXPECT_FLOAT_EQ(g[3], 0.0f);
}

// ── Crossfade pool edge cases ────────────────────────────────────────────────

TEST(Granular, SamplePlayerFourDiscontinuitiesInOneFadeStealTheOldestTail) {
    // Bank k = ramp + 1000k. Three bank switches 64 frames apart, then a
    // retrigger: the fourth discontinuity finds no free tail slot and steals
    // the one closest to done (the first). Every tail is parked with the
    // live head's fade-in gain at that moment: sin(64 theta) for all three
    // later ones.
    std::vector<thl::core::BufferF> banks;
    for (int k = 0; k < 4; ++k) {
        thl::core::BufferF b(1, 10000, k_sample_rate);
        float* d = b.get_write_pointer(0);
        for (size_t i = 0; i < 10000; ++i) { d[i] = static_cast<float>(i) + 1000.0f * k; }
        banks.push_back(std::move(b));
    }
    PlayerRig rig(std::move(banks));
    rig.render(2048);
    rig.m_params.m_sample_index = 1;
    rig.render(64);
    rig.m_params.m_sample_index = 2;
    rig.render(64);
    rig.m_params.m_sample_index = 3;
    rig.render(64);
    rig.m_player.note_on();
    rig.render(512);

    float const parked = gain_in(64);
    for (size_t j = 0; j < k_fade; ++j) {
        float expected = gain_in(j) * static_cast<float>(3000 + j);       // live: bank 3 from Start
        expected += parked * gain_out(j) * static_cast<float>(5240 + j);  // bank 3 at the head
        if (64 + j < k_fade) {
            expected += parked * gain_out(64 + j) * static_cast<float>(4240 + j);
        }
        if (128 + j < k_fade) {
            expected += parked * gain_out(128 + j) * static_cast<float>(3240 + j);
        }
        // bank 0's tail (parked at 2048, gain 1) was stolen: no term.
        ASSERT_NEAR(rig.m_out[2240 + j], expected, 0.1f) << "j=" << j;
    }
}

TEST(Granular, SamplePlayerTailWhoseBankWasUnloadedReadsSilence) {
    std::vector<thl::core::BufferF> banks;
    banks.push_back(make_ramp(1, 10000));
    thl::core::BufferF plus(1, 10000, k_sample_rate);
    float* d = plus.get_write_pointer(0);
    for (size_t i = 0; i < 10000; ++i) { d[i] = static_cast<float>(i) + 1000.0f; }
    banks.push_back(std::move(plus));
    PlayerRig rig(std::move(banks));
    rig.render(2048);
    rig.m_params.m_sample_index = 1;  // bank 0 becomes the tail
    rig.render(64);

    // Reload with bank 0 emptied; the parked tail must go silent, the live
    // head on bank 1 continues its fade-in.
    std::vector<thl::core::BufferF> reloaded;
    reloaded.emplace_back();
    thl::core::BufferF plus2(1, 10000, k_sample_rate);
    float* d2 = plus2.get_write_pointer(0);
    for (size_t i = 0; i < 10000; ++i) { d2[i] = static_cast<float>(i) + 1000.0f; }
    reloaded.push_back(std::move(plus2));
    load(rig.m_store, std::move(reloaded));
    rig.render(512);
    for (size_t j = 0; j < k_fade - 64; ++j) {
        float const expected = gain_in(64 + j) * static_cast<float>(3112 + j);
        ASSERT_NEAR(rig.m_out[2112 + j], expected, 0.05f) << "j=" << j;
    }

    // Empty the live bank too: silence, head reported finished.
    std::vector<thl::core::BufferF> gone(2);
    load(rig.m_store, std::move(gone));
    rig.render(64);
    EXPECT_FALSE(rig.m_player.is_started());
    EXPECT_EQ(rig.m_listener.m_finished.size(), 1u);
    for (size_t j = 0; j < 64; ++j) { ASSERT_FLOAT_EQ(rig.m_out[2624 + j], 0.0f); }
}

TEST(Granular, SamplePlayerRegionSmallerThanLoopFloorLoopsWhole) {
    // A 500-frame region is below the two-crossfade floor (960): it loops
    // whole, wrapping every 500 frames with the crossfade riding across.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_end = 0.05f;
    rig.m_params.m_sample_loop_point = 0.03f;  // ignored: floored to Start
    rig.render(1024);
    for (size_t k = 0; k < k_fade; ++k) {
        float const expected =
            gain_in(k) * static_cast<float>(k) + gain_out(k) * static_cast<float>(500 + k);
        ASSERT_NEAR(rig.m_out[500 + k], expected, 0.05f) << "k=" << k;
    }
}

// ── Block shapes ─────────────────────────────────────────────────────────────

TEST(Granular, EnginesTolerateFewerOrMoreBlockChannelsThanPrepared) {
    PlayerRig rig({make_ramp(1, 10000)});
    Block mono(1, k_block);
    rig.m_player.render(mono.m_view, rig.m_params);  // channel 1 pointer is null
    for (size_t i = 0; i < k_block; ++i) {
        ASSERT_FLOAT_EQ(mono.m_data[0][i], static_cast<float>(i));
    }
    Block quad(4, k_block);
    rig.m_player.render(quad.m_view, rig.m_params);
    for (size_t i = 0; i < k_block; ++i) {
        ASSERT_FLOAT_EQ(quad.m_data[0][i], static_cast<float>(k_block + i));
        ASSERT_FLOAT_EQ(quad.m_data[2][i], 0.0f);
        ASSERT_FLOAT_EQ(quad.m_data[3][i], 0.0f);
    }

    GrainVisualizer viz;
    GrainEngine engine(rig.m_reader, viz);
    engine.prepare(k_sample_rate, 2);
    VoiceParams params;
    params.m_channel_mode = ChannelMode::TrueStereo;
    Block mono2(1, k_block);
    engine.render(mono2.m_view, params, EngineMode::GranularLoop, 0);
    Block quad2(4, k_block);
    engine.render(quad2.m_view, params, EngineMode::GranularLoop, k_block);
    for (size_t i = 0; i < k_block; ++i) {
        ASSERT_TRUE(std::isfinite(mono2.m_data[0][i]));
        ASSERT_FLOAT_EQ(quad2.m_data[3][i], 0.0f);
    }
}

// ── The voice facade ─────────────────────────────────────────────────────────

namespace {

// GrainProcessorImpl's parameter hooks are pure virtual: serve them from
// plain arrays.
class StubVoice final : public GrainProcessorImpl {
public:
    explicit StubVoice(thl::dsp::audio::AudioDataStore& store) : GrainProcessorImpl(store) {
        m_f[Volume] = 1.0f;
        m_f[Velocity] = 1.0f;
        m_f[SampleEnd] = 1.0f;
        m_f[EnvelopeSustain] = 1.0f;
        m_f[Size] = 0.5f;
        m_f[Density] = 0.5f;
        m_i[ChannelModeParam] = static_cast<int>(ChannelMode::TrueStereo);
        m_i[EngineModeParam] = static_cast<int>(EngineMode::GranularLoop);
    }
    bool m_playing{false};
    void set_mode(EngineMode mode) { m_i[EngineModeParam] = static_cast<int>(mode); }

    std::vector<float> run(size_t frames) {
        std::vector<float> out;
        Block block(2, k_block);
        for (size_t done = 0; done < frames; done += k_block) {
            block.clear();
            thl::core::BufferView view(block.m_view.m_channels.data(), 2, k_block);
            process(view);
            out.insert(out.end(), block.m_data[0].begin(), block.m_data[0].end());
        }
        return out;
    }

private:
    std::array<float, NumParameters> m_f{};
    std::array<int, NumParameters> m_i{};
    float get_parameter_float(Parameter p, uint32_t) override { return m_f[p]; }
    bool get_parameter_bool(Parameter p, uint32_t) override { return p == Playing && m_playing; }
    int get_parameter_int(Parameter p, uint32_t) override { return m_i[p]; }
};

}  // namespace

TEST(Granular, VoicePrepareSeedsTheActiveModeFromTheParameter) {
    // A preset that boots in Sample mode must not run a block of grains
    // first: the very first block is the head (ramp), not grain-windowed.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        b.push_back(make_ramp(1, 96000));
        return b;
    }());
    StubVoice voice(store);
    voice.set_mode(EngineMode::Sample);
    voice.prepare(k_sample_rate, k_block, 2);
    voice.m_playing = true;
    auto const out = voice.run(k_block);
    // The ADSR floors attack at 0.1 ms (~5 samples); after that out == n.
    for (size_t n = 8; n < k_block; ++n) { ASSERT_NEAR(out[n], static_cast<float>(n), 1e-3f); }
}

TEST(Granular, VoiceModeSwitchFadesThroughZeroAndRevertsMidFade) {
    // DC source in Sample mode: the output IS the mode gain. Requesting
    // another mode ramps it linearly to 0 over 15 ms (720 frames); revoking
    // the request mid-fade ramps straight back up with no switch.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        thl::core::BufferF dc(1, 96000, k_sample_rate);
        std::fill_n(dc.get_write_pointer(0), 96000, 1.0f);
        b.push_back(std::move(dc));
        return b;
    }());
    StubVoice voice(store);
    voice.set_mode(EngineMode::Sample);
    voice.prepare(k_sample_rate, k_block, 2);
    voice.m_playing = true;
    voice.run(1024);  // sounding, gain 1

    constexpr float k_step = 1.0f / 720.0f;
    voice.set_mode(EngineMode::GranularLoop);
    auto down = voice.run(256);
    for (size_t k = 0; k < 256; ++k) {
        ASSERT_NEAR(down[k], 1.0f - static_cast<float>(k + 1) * k_step, 1e-4f) << "k=" << k;
    }
    voice.set_mode(EngineMode::Sample);  // revert
    auto up = voice.run(512);
    float const from = 1.0f - 256.0f * k_step;
    for (size_t k = 0; k < 256; ++k) {
        ASSERT_NEAR(up[k], std::min(1.0f, from + static_cast<float>(k + 1) * k_step), 1e-4f)
            << "k=" << k;
    }
    for (size_t k = 300; k < 512; ++k) { ASSERT_NEAR(up[k], 1.0f, 1e-5f); }

    // A full fade: silence is reached, then the switch happens and gain
    // ramps back with the grain engine underneath.
    voice.set_mode(EngineMode::GranularLoop);
    auto full = voice.run(1024);
    for (size_t k = 0; k < 720; ++k) {
        ASSERT_NEAR(full[k], 1.0f - static_cast<float>(k + 1) * k_step, 1e-4f) << "k=" << k;
    }
    for (size_t k = 720; k < 768; ++k) { ASSERT_NEAR(full[k], 0.0f, 1e-5f) << "k=" << k; }
}
