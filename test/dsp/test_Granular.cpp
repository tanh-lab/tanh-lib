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
#include <tanh/dsp/granular/SliceMap.h>
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
    params.m_spray = 1.0f;  // the window past the edge clips, never wraps
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
    // Position at the very end with a forward spray: everything clips to
    // the last frame instead of wrapping to the start.
    params.m_position = 1.0f;
    params.m_spray = 1.0f;
    params.m_tilt = 1.0f;
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(head.pick_start(region, 0.0f, 0, params, rng), FramePos{96000 - 1});
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

        ASSERT_EQ(listener.m_dropped, 0u) << "listener capacity outgrown at density " << density;
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
    auto r = SampleRegion::from_normalized(0.5f, 0.25f, 0.0f, 1000);  // end < start: reverse
    EXPECT_TRUE(r.m_reverse);
    EXPECT_EQ(r.size(), 250u);
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
    EXPECT_EQ(rig.m_listener.m_dropped, 0u);
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
    bool m_loop{true};
    bool m_slicer{false};
    bool m_has_map{false};
    SliceMap m_map{};
    void set_mode(EngineMode mode) { m_i[EngineModeParam] = static_cast<int>(mode); }
    void set_sample_start(float v) { m_f[SampleStart] = v; }
    void set_sample_end(float v) { m_f[SampleEnd] = v; }
    void set_release(float v) { m_f[EnvelopeRelease] = v; }
    bool read_slice_map(SliceMap& out) override {
        if (!m_has_map) { return false; }
        out = m_map;
        return true;
    }

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
    bool get_parameter_bool(Parameter p, uint32_t) override {
        if (p == LoopEnabled) { return m_loop; }
        if (p == SlicerEnabled) { return m_slicer; }
        return p == Playing && m_playing;
    }
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

// ── Reverse (End before Start) ───────────────────────────────────────────────

TEST(Granular, SampleRegionEndBeforeStartReversesAndMirrorsTheLoop) {
    auto r = SampleRegion::from_normalized(0.8f, 0.2f, 0.7f, 1000);
    EXPECT_TRUE(r.m_reverse);
    EXPECT_EQ(r.m_start, 200u);
    EXPECT_EQ(r.m_end, 800u);
    // Loop at 700 physical (near Start = 800) re-enters near the virtual
    // start: 200 + 800 - 700 - 1 = 299.
    EXPECT_EQ(r.m_loop_point, 299u);
    EXPECT_DOUBLE_EQ(r.physical(200.0), 799.0);
    EXPECT_DOUBLE_EQ(r.physical(799.0), 200.0);
    auto f = SampleRegion::from_normalized(0.2f, 0.8f, 0.7f, 1000);
    EXPECT_FALSE(f.m_reverse);
    EXPECT_EQ(f.m_loop_point, 700u);
}

TEST(Granular, SamplePlayerReverseRegionPlaysBackwardsAndLoopsAtTheMirroredPoint) {
    // Start 0.48 (4800), End 0 → the head enters at 4799 and runs down to 0,
    // then re-enters at Loop = 0.38 (physical 3800), running down again.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_start = 0.48f;
    rig.m_params.m_sample_end = 0.0f;
    rig.m_params.m_sample_loop_point = 0.38f;
    rig.render(6016);

    for (size_t n = 0; n < 4800; ++n) {
        ASSERT_NEAR(rig.m_out[n], static_cast<float>(4799 - n), 1e-3f) << "n=" << n;
    }
    // Wrap: the tail keeps running down past 0 (clamped at frame 0), the
    // live head fades in from 3799 downwards.
    for (size_t k = 0; k < k_fade; ++k) {
        float const tail =
            static_cast<float>(std::max(0.0, 4799.0 - 4800.0 - static_cast<double>(k)));
        float const expected = gain_in(k) * static_cast<float>(3800 - k) + gain_out(k) * tail;
        ASSERT_NEAR(rig.m_out[4800 + k], expected, 0.05f) << "k=" << k;
    }
    for (size_t k = k_fade; k < 1200; ++k) {
        ASSERT_NEAR(rig.m_out[4800 + k], static_cast<float>(3800 - k), 1e-3f) << "k=" << k;
    }
}

TEST(Granular, GrainEngineReverseRegionGrainsReadBackwards) {
    // Rectangle window, one grain at a time, region reversed: the grain
    // enters at the mirrored frame and its samples descend.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        b.push_back(make_ramp(1, 96000));
        return b;
    }());
    SampleReader reader(store);
    GrainVisualizer viz;
    GrainEngine engine(reader, viz);
    engine.prepare(k_sample_rate, 2);
    engine.reset_schedule(EngineMode::GranularLoop);

    VoiceParams params;
    params.m_channel_mode = ChannelMode::TrueStereo;
    params.m_density = 0.0f;       // one grain per 24000 frames
    params.m_size = 0.0f;          // 96-frame grains
    params.m_window_shape = 0.0f;  // rectangle: output == source
    params.m_sample_start = 0.5f;  // 48000
    params.m_sample_end = 0.25f;   // 24000 -> reversed, entry at 47999

    Block block(2, k_block);
    block.clear();
    engine.render(block.m_view, params, EngineMode::GranularLoop, 0);
    for (size_t i = 1; i < k_block; ++i) {
        ASSERT_NEAR(block.m_data[0][i], static_cast<float>(47999 - i), 1e-2f) << "i=" << i;
    }
}

// ── Slicing ──────────────────────────────────────────────────────────────────

namespace {

// Six uneven slices: the fixture the design doc draws.
SliceMap six_slices() {
    SliceMap m;
    m.m_count = 6;
    m.m_bounds = {0.0f, 0.17f, 0.29f, 0.52f, 0.71f, 0.86f, 1.0f};
    return m;
}

// Which slice a physical frame lies in.
int slice_at(const SliceMap& m, FramePos frame, size_t total) {
    for (int k = 0; k < m.m_count; ++k) {
        if (frame < static_cast<FramePos>(m.boundary_frame(k + 1, total))) { return k; }
    }
    return m.m_count - 1;
}

}  // namespace

TEST(Granular, SliceMapStepsFloorRoundAndClamp) {
    auto const m = six_slices();
    EXPECT_TRUE(m.valid());
    // Position: floor(u·6), clamped into 0..5.
    EXPECT_EQ(m.slice_of_step(0.0f), 0);
    EXPECT_EQ(m.slice_of_step(0.16f), 0);
    EXPECT_EQ(m.slice_of_step(0.17f), 1);
    EXPECT_EQ(m.slice_of_step(0.52f), 3);  // 52 % -> slice 4 of 6 (0-based 3)
    EXPECT_EQ(m.slice_of_step(1.0f), 5);
    EXPECT_EQ(m.slice_of_step(7.0f), 5);
    EXPECT_EQ(m.slice_of_step(-1.0f), 0);
    // Start / End: round(u·6), clamped into 0..6.
    EXPECT_EQ(m.boundary_of_step(0.0f), 0);
    EXPECT_EQ(m.boundary_of_step(0.08f), 0);
    EXPECT_EQ(m.boundary_of_step(0.09f), 1);
    EXPECT_EQ(m.boundary_of_step(0.5f), 3);
    EXPECT_EQ(m.boundary_of_step(1.0f), 6);
    EXPECT_EQ(m.boundary_of_step(2.0f), 6);
    // Frames.
    EXPECT_EQ(m.boundary_frame(0, 1000), 0u);
    EXPECT_EQ(m.boundary_frame(2, 1000), 290u);
    EXPECT_EQ(m.boundary_frame(6, 1000), 1000u);
    EXPECT_EQ(m.boundary_frame(9, 1000), 1000u);
    // Validity.
    SliceMap bad = m;
    bad.m_bounds[3] = bad.m_bounds[2];
    EXPECT_FALSE(bad.valid());
    bad = m;
    bad.m_count = 1;
    EXPECT_FALSE(bad.valid());
    bad = m;
    bad.m_bounds[6] = 0.99f;
    EXPECT_FALSE(bad.valid());
    EXPECT_FALSE(SliceMap{}.valid());
    EXPECT_TRUE(SliceMap::grid(4).valid());
    EXPECT_EQ(SliceMap::grid(40).m_count, k_max_slices);
    EXPECT_EQ(SliceMap::grid(1).m_count, k_min_slices);
}

TEST(Granular, SliceMapStepToNormIsMonotonicThroughUnevenBounds) {
    auto const m = six_slices();
    EXPECT_FLOAT_EQ(m.step_to_norm(0.0), 0.0f);
    EXPECT_FLOAT_EQ(m.step_to_norm(6.0), 1.0f);
    EXPECT_FLOAT_EQ(m.step_to_norm(3.0), 0.52f);
    EXPECT_NEAR(m.step_to_norm(2.5), 0.29f + 0.5f * (0.52f - 0.29f), 1e-6f);
    EXPECT_FLOAT_EQ(m.step_to_norm(-1.0), 0.0f);
    EXPECT_FLOAT_EQ(m.step_to_norm(9.0), 1.0f);
    float last = -1.0f;
    for (int i = 0; i <= 600; ++i) {
        float const v = m.step_to_norm(static_cast<double>(i) / 100.0);
        EXPECT_GE(v, last);
        last = v;
    }
}

TEST(Granular, SampleRegionFromSlicesSnapsToBoundaries) {
    auto const m = six_slices();
    // Start near boundary 2 (0.29), End near boundary 5 (0.86): slices 2-4.
    auto r = SampleRegion::from_slices(0.3f, 0.8f, m, 1000);
    EXPECT_FALSE(r.m_reverse);
    EXPECT_EQ(r.m_start, 290u);
    EXPECT_EQ(r.m_end, 860u);
    EXPECT_EQ(r.m_loop_point, 290u);  // no Loop marker: repeats from Start
    // Full range.
    r = SampleRegion::from_slices(0.0f, 1.0f, m, 1000);
    EXPECT_EQ(r.m_start, 0u);
    EXPECT_EQ(r.m_end, 1000u);
}

TEST(Granular, SampleRegionFromSlicesEndBeforeStartReverses) {
    auto const m = six_slices();
    auto r = SampleRegion::from_slices(0.8f, 0.3f, m, 1000);
    EXPECT_TRUE(r.m_reverse);
    EXPECT_EQ(r.m_start, 290u);
    EXPECT_EQ(r.m_end, 860u);
    EXPECT_EQ(r.m_loop_point, 290u);
    // The virtual start reads the Start marker's side (boundary 5, minus 1).
    EXPECT_DOUBLE_EQ(r.physical(290.0), 859.0);
}

TEST(Granular, SampleRegionFromSlicesEqualStartEndPlaysOneSlice) {
    auto const m = six_slices();
    auto r = SampleRegion::from_slices(0.5f, 0.5f, m, 1000);  // both on boundary 3
    EXPECT_FALSE(r.m_reverse);
    EXPECT_EQ(r.m_start, 520u);
    EXPECT_EQ(r.m_end, 710u);
    // Both on the last boundary: the last slice.
    r = SampleRegion::from_slices(1.0f, 1.0f, m, 1000);
    EXPECT_EQ(r.m_start, 860u);
    EXPECT_EQ(r.m_end, 1000u);
    EXPECT_GT(r.size(), 0u);
}

TEST(Granular, PositionSprayHeadSliceSprayZeroStartsOnSliceStart) {
    PositionSprayHead head;
    std::mt19937 rng(3);
    VoiceParams params;
    params.m_slicer = true;
    params.m_slices = six_slices();
    params.m_position = 0.4f;  // slice 3 (0-based 2) starts at 0.29
    params.m_spray = 0.0f;
    auto const region = SampleRegion::full(100000);
    auto const expected = static_cast<FramePos>(0.29f * static_cast<float>(100000 - 1));
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(head.pick_start(region, 0.0f, 0, params, rng), expected);
    }
    // Temperature at Spray 0 stays inside the chosen slice.
    for (int i = 0; i < 500; ++i) {
        FramePos const f = head.pick_start(region, 1.0f, 0, params, rng);
        EXPECT_EQ(slice_at(params.m_slices, f, 100000), 2) << "f=" << f;
    }
}

TEST(Granular, PositionSprayHeadSliceTiltRightCoversChosenAndNext) {
    PositionSprayHead head;
    std::mt19937 rng(5);
    VoiceParams params;
    params.m_slicer = true;
    params.m_slices = six_slices();
    params.m_position = 0.4f;      // slice index 2
    params.m_spray = 2.0f / 6.0f;  // two slices wide
    params.m_tilt = 1.0f;          // forward only: slices 2 and 3
    auto const region = SampleRegion::full(100000);
    std::array<int, 6> hist{};
    for (int i = 0; i < 2000; ++i) {
        hist[static_cast<size_t>(
            slice_at(params.m_slices, head.pick_start(region, 0.0f, 0, params, rng), 100000))]++;
    }
    EXPECT_EQ(hist[0] + hist[1] + hist[4] + hist[5], 0);
    // Equal share per slice, however long each is in time.
    EXPECT_GT(hist[2], 800);
    EXPECT_GT(hist[3], 800);
    // Spray 1/6, Tilt right: the chosen slice only, spread across it.
    params.m_spray = 1.0f / 6.0f;
    hist = {};
    FramePos lo = 1 << 30, hi = -1;
    for (int i = 0; i < 1000; ++i) {
        FramePos const f = head.pick_start(region, 0.0f, 0, params, rng);
        hist[static_cast<size_t>(slice_at(params.m_slices, f, 100000))]++;
        lo = std::min(lo, f);
        hi = std::max(hi, f);
    }
    EXPECT_EQ(hist[2], 1000);
    EXPECT_GT(hi - lo, static_cast<FramePos>(0.2f * 100000 * 0.9f));  // nearly the whole slice
}

TEST(Granular, PositionSprayHeadSliceTiltLeftExcludesChosenSlice) {
    PositionSprayHead head;
    std::mt19937 rng(9);
    VoiceParams params;
    params.m_slicer = true;
    params.m_slices = six_slices();
    params.m_position = 0.4f;      // slice index 2
    params.m_spray = 2.0f / 6.0f;  // two slices
    params.m_tilt = -1.0f;         // the two before: 0 and 1
    auto const region = SampleRegion::full(100000);
    std::array<int, 6> hist{};
    for (int i = 0; i < 2000; ++i) {
        hist[static_cast<size_t>(
            slice_at(params.m_slices, head.pick_start(region, 0.0f, 0, params, rng), 100000))]++;
    }
    EXPECT_EQ(hist[2] + hist[3] + hist[4] + hist[5], 0);
    EXPECT_GT(hist[0], 800);
    EXPECT_GT(hist[1], 800);
    // Centred, Spray 1/6: one step each way -> slices 1 and 2.
    params.m_tilt = 0.0f;
    params.m_spray = 1.0f / 6.0f;
    hist = {};
    for (int i = 0; i < 2000; ++i) {
        hist[static_cast<size_t>(
            slice_at(params.m_slices, head.pick_start(region, 0.0f, 0, params, rng), 100000))]++;
    }
    EXPECT_EQ(hist[0] + hist[3] + hist[4] + hist[5], 0);
    EXPECT_GT(hist[1], 800);
    EXPECT_GT(hist[2], 800);
}

TEST(Granular, PositionSprayHeadSliceWindowWrapsPastLastSlice) {
    PositionSprayHead head;
    std::mt19937 rng(11);
    VoiceParams params;
    params.m_slicer = true;
    params.m_slices = six_slices();
    params.m_position = 0.9f;      // slice index 5 (the last)
    params.m_spray = 2.0f / 6.0f;  // the last slice and, wrapped, the first
    params.m_tilt = 1.0f;
    auto const region = SampleRegion::full(100000);
    std::array<int, 6> hist{};
    for (int i = 0; i < 2000; ++i) {
        hist[static_cast<size_t>(
            slice_at(params.m_slices, head.pick_start(region, 0.0f, 0, params, rng), 100000))]++;
    }
    EXPECT_EQ(hist[1] + hist[2] + hist[3] + hist[4], 0);
    EXPECT_GT(hist[5], 800);
    EXPECT_GT(hist[0], 800);
    // 100 % forward: every slice once.
    params.m_spray = 1.0f;
    hist = {};
    for (int i = 0; i < 6000; ++i) {
        hist[static_cast<size_t>(
            slice_at(params.m_slices, head.pick_start(region, 0.0f, 0, params, rng), 100000))]++;
    }
    for (int k = 0; k < 6; ++k) { EXPECT_GT(hist[static_cast<size_t>(k)], 700) << "k=" << k; }
}

TEST(Granular, SamplePlayerOneShotStopsAtEndWithTailFade) {
    // Region [0, 4800), Loop off: the head plays to End once, the outgoing
    // tail rides out the 10 ms fade past End and then everything is silent.
    PlayerRig rig({make_ramp(1, 10000)});
    rig.m_params.m_sample_end = 0.48f;
    rig.m_params.m_loop = false;
    rig.render(6016);
    for (size_t n = 0; n < 4800; ++n) {
        ASSERT_NEAR(rig.m_out[n], static_cast<float>(n), 1e-3f) << "n=" << n;
    }
    EXPECT_TRUE(rig.m_player.finished());
    for (size_t k = 0; k < k_fade; ++k) {
        float const expected = gain_out(k) * static_cast<float>(4800 + k);
        ASSERT_NEAR(rig.m_out[4800 + k], expected, 0.05f) << "k=" << k;
    }
    for (size_t k = k_fade; k < 1200; ++k) {
        ASSERT_NEAR(rig.m_out[4800 + k], 0.0f, 1e-5f) << "k=" << k;
    }
    // note_on brings the head back from the region start.
    rig.m_player.note_on();
    rig.render(k_block);
    EXPECT_FALSE(rig.m_player.finished());
    for (size_t n = 8; n < k_block; ++n) {
        ASSERT_NEAR(rig.m_out[6016 + n], static_cast<float>(n), 0.5f) << "n=" << n;
    }
}

TEST(Granular, GrainEngineLoopOneShotStopsTriggeringAtEnd) {
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
    params.m_density = 1.0f;  // 480-frame interval
    params.m_size = 0.0f;
    params.m_sample_end = 0.1f;  // 9600 frames: 20 triggers, then done
    params.m_loop = false;

    Block block(2, k_block);
    size_t elapsed = 0;
    while (elapsed < 48000) {
        block.clear();
        engine.render(block.m_view, params, EngineMode::GranularLoop, elapsed);
        elapsed += k_block;
    }
    EXPECT_TRUE(engine.finished(EngineMode::GranularLoop));
    EXPECT_FALSE(engine.finished(EngineMode::GranularPosition));
    EXPECT_EQ(listener.m_triggered.size(), 20u);
    for (auto const& t : listener.m_triggered) { EXPECT_LT(t.m_pos, 0.1f); }
    // Looping again: the scan restarts and keeps going.
    engine.reset_schedule(EngineMode::GranularLoop);
    EXPECT_FALSE(engine.finished(EngineMode::GranularLoop));
    params.m_loop = true;
    listener.m_triggered.clear();
    elapsed = 0;
    while (elapsed < 48000) {
        block.clear();
        engine.render(block.m_view, params, EngineMode::GranularLoop, elapsed);
        elapsed += k_block;
    }
    EXPECT_EQ(listener.m_triggered.size(), 100u);
}

TEST(Granular, VoiceOneShotReleasesAndDoesNotRetriggerWhileGateHeld) {
    // Sample mode, Loop off, region [0, 4800). The head reaches End after
    // 4800 frames: the envelope releases and, with the gate still held, the
    // voice must stay silent instead of re-triggering. Releasing and
    // pressing again replays.
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
    voice.set_sample_end(0.05f);  // 4800 frames
    voice.set_release(0.001f);    // release ~48 frames
    voice.m_loop = false;
    voice.prepare(k_sample_rate, k_block, 2);
    voice.m_playing = true;
    auto out = voice.run(4800 + 4800);
    ASSERT_NEAR(out[2400], 1.0f, 1e-3f);
    // Well after End (tail + release are < 600 frames): silence, and it stays.
    for (size_t n = 4800 + 1200; n < 9600; ++n) { ASSERT_NEAR(out[n], 0.0f, 1e-4f) << "n=" << n; }
    EXPECT_FALSE(voice.is_active());
    // Still held: nothing.
    out = voice.run(2400);
    for (float v : out) { ASSERT_NEAR(v, 0.0f, 1e-4f); }
    // Gate falls and rises: the head plays again from Start.
    voice.m_playing = false;
    voice.run(k_block);
    voice.m_playing = true;
    out = voice.run(2400);
    EXPECT_TRUE(voice.is_active());
    ASSERT_NEAR(out[1200], 1.0f, 1e-3f);
}

TEST(Granular, VoiceWithoutSliceMapIgnoresSlicerFlag) {
    // SlicerEnabled set but no map from the host: Sample mode plays the plain
    // Start..End region (the ramp from 0), exactly as with the flag off.
    thl::dsp::audio::AudioDataStore store;
    load(store, [] {
        std::vector<thl::core::BufferF> b;
        b.push_back(make_ramp(1, 96000));
        return b;
    }());
    StubVoice voice(store);
    voice.set_mode(EngineMode::Sample);
    voice.m_slicer = true;
    voice.m_has_map = false;
    voice.prepare(k_sample_rate, k_block, 2);
    voice.m_playing = true;
    auto out = voice.run(k_block);
    for (size_t n = 8; n < k_block; ++n) { ASSERT_NEAR(out[n], static_cast<float>(n), 1e-3f); }

    // With a map, Start 0.5 / End 1 snap to boundary 3 of the six: the head
    // enters at frame 0.52 * 96000.
    StubVoice sliced(store);
    sliced.set_mode(EngineMode::Sample);
    sliced.m_slicer = true;
    sliced.m_has_map = true;
    sliced.m_map = six_slices();
    sliced.set_sample_start(0.5f);
    sliced.prepare(k_sample_rate, k_block, 2);
    sliced.m_playing = true;
    out = sliced.run(k_block);
    auto const entry = static_cast<float>(sliced.m_map.boundary_frame(3, 96000));
    for (size_t n = 8; n < k_block; ++n) {
        ASSERT_NEAR(out[n], entry + static_cast<float>(n), 1e-2f) << "n=" << n;
    }
}
