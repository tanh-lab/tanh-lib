// Blocks larger than the prepared maximum. MotionRecorder and XYController run
// such a block as consecutive chunks of at most the prepared size, so the result
// is bit-identical to the same audio in prepared-size blocks (the reference
// below), and their clocks stay with the transport. The matrix itself requires
// num_samples <= samples_per_block (its buffers have that length).

#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/MotionLane.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/XYController.h>
#include <tanh/modulation/XYPad.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "MotionRig.h"
#include "XYControllerRig.h"

using namespace thl::modulation;
using thl::dsp::transport::TransportInfo;
using xy_test::FakeBackend;
using xy_test::k_sr;
using xy_test::SimTransport;

namespace {

constexpr uint32_t k_bs = 256;    // prepared maximum
constexpr uint32_t k_big = 1000;  // host block: 256 + 256 + 256 + 232

/// Pad-style input of any length: a finger touching [m_down, m_up), moving.
struct Gesture {
    uint64_t m_down = 100;
    uint64_t m_up = 60000;

    [[nodiscard]] bool touched(uint64_t s) const { return s >= m_down && s < m_up; }
    [[nodiscard]] static float x(uint64_t s) {
        return 0.2f + (0.6f * static_cast<float>((s % 20000) / 20000.0));
    }
    [[nodiscard]] static float y(uint64_t s) {
        return 0.5f + (0.3f * static_cast<float>(std::sin(static_cast<double>(s) / 7000.0)));
    }
};

/// Owns the arrays of one XYPadStream that starts at sample @p start.
struct Stream {
    Stream(const Gesture& g, uint64_t start, uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t s = start + i;
            const bool on = g.touched(s);
            m_x.push_back(on ? Gesture::x(s) : 0.0f);
            m_y.push_back(on ? Gesture::y(s) : 0.0f);
            m_active.push_back(on ? 1 : 0);
            // An event every 64 samples while touched, and at both edges.
            if (on != g.touched(s - 1) || (on && s % 64 == 0)) { m_cps.push_back(i); }
        }
    }
    [[nodiscard]] XYPadStream view() const {
        return XYPadStream{.m_x = m_x.data(),
                           .m_y = m_y.data(),
                           .m_active = m_active.data(),
                           .m_change_points = m_cps,
                           .m_num_samples = static_cast<uint32_t>(m_x.size())};
    }
    std::vector<float> m_x, m_y;
    std::vector<uint8_t> m_active;
    std::vector<uint32_t> m_cps;
};

TransportInfo playing(double beat, uint32_t n, double bpm = 120.0) {
    TransportInfo t;
    t.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                TransportInfo::k_is_playing;
    t.m_bpm = bpm;
    t.m_beat_position = beat;
    t.m_beats_per_sample = bpm / (60.0 * k_sr);
    t.m_num_samples = n;
    return t;
}

/// The prepared-size chunks of a host block, built the way a clock running at
/// k_bs would report them: the beat continues, the flags sit on the first chunk.
std::vector<TransportInfo> chunks_of(const TransportInfo& host) {
    std::vector<TransportInfo> out;
    for (uint32_t off = 0; off < host.m_num_samples; off += k_bs) {
        TransportInfo t = host;
        t.m_beat_position =
            host.m_beat_position + (static_cast<double>(off) * host.m_beats_per_sample);
        t.m_num_samples = std::min(k_bs, host.m_num_samples - off);
        if (off > 0) {
            t.m_flags &= ~TransportInfo::k_discontinuity_mask;
            t.m_jump_delta_beats = 0.0;
        }
        out.push_back(t);
    }
    return out;
}

struct RecorderOutput {
    std::vector<float> m_x, m_y;
    std::vector<uint8_t> m_gate;
    std::vector<uint32_t> m_cps;

    void append(const MotionRecorder& r) {
        const uint32_t n = r.num_samples();
        m_x.insert(m_x.end(), r.out_x(), r.out_x() + n);
        m_y.insert(m_y.end(), r.out_y(), r.out_y() + n);
        m_gate.insert(m_gate.end(), r.out_gate(), r.out_gate() + n);
        const auto cps = r.change_points();
        m_cps.insert(m_cps.end(), cps.begin(), cps.end());
    }
};

void expect_identical(const RecorderOutput& a, const RecorderOutput& b) {
    ASSERT_EQ(a.m_x.size(), b.m_x.size());
    for (size_t i = 0; i < a.m_x.size(); ++i) {
        ASSERT_EQ(a.m_x[i], b.m_x[i]) << i;
        ASSERT_EQ(a.m_y[i], b.m_y[i]) << i;
        ASSERT_EQ(a.m_gate[i], b.m_gate[i]) << i;
    }
    EXPECT_EQ(a.m_cps, b.m_cps);
}

/// Recorder A gets host blocks as they come (some oversized), recorder B the
/// same audio in prepared-size chunks. Records the outputs of every call.
struct RecorderPair {
    RecorderPair() {
        m_a.prepare(k_sr, k_bs);
        m_b.prepare(k_sr, k_bs);
    }

    void block(const TransportInfo& host, bool record_all_of_b = true) {
        const Stream in(m_gesture, m_now, host.m_num_samples);
        m_a.process(host, in.view(), host.m_num_samples);
        m_out_a.append(m_a);
        uint64_t s = m_now;
        for (const TransportInfo& t : chunks_of(host)) {
            const Stream part(m_gesture, s, t.m_num_samples);
            m_b.process(t, part.view(), t.m_num_samples);
            // B's last chunk is what A's outputs hold after an oversized block.
            if (record_all_of_b || s + t.m_num_samples == m_now + host.m_num_samples) {
                m_out_b.append(m_b);
            }
            s += t.m_num_samples;
        }
        m_beat = host.beat_end();
        m_now += host.m_num_samples;
    }

    Gesture m_gesture;
    MotionRecorder m_a, m_b;
    RecorderOutput m_out_a, m_out_b;
    uint64_t m_now = 0;
    double m_beat = 0.0;
};

}  // namespace

// ── MotionRecorder ────────────────────────────────────────────────────────────

// A bar take that starts inside an oversized block, records across further
// oversized blocks and plays back: bit-identical to prepared-size blocks.
TEST(OversizedBlocks, RecorderTakeAndPlaybackMatchPreparedChunks) {
    RecorderPair p;
    ASSERT_TRUE(p.m_a.arm(LoopLength::Bars1));
    ASSERT_TRUE(p.m_b.arm(LoopLength::Bars1));

    // 1 bar at 120 BPM = 96000 samples; the touch lasts 60000 samples from 100.
    // Mix oversized and normal blocks; service() publishes the finished take.
    int b = 0;
    while (p.m_now < 4 * 96000) {
        const uint32_t n = (b % 3 == 0) ? k_big : (b % 3 == 1 ? k_bs : 777);
        TransportInfo t = playing(p.m_beat, n);
        if (b == 0) { t.m_flags |= TransportInfo::k_timeline_reset; }
        // Only A's last chunk is comparable inside an oversized block.
        p.block(t, /*record_all_of_b=*/false);
        if (b % 50 == 0) {
            p.m_a.service();
            p.m_b.service();
        }
        ++b;
    }
    expect_identical(p.m_out_a, p.m_out_b);

    // The take finished, plays, and both recorders agree on every bit of state.
    EXPECT_EQ(p.m_a.state(), MotionState::Playing);
    EXPECT_NE(p.m_a.playing_take_id(), 0u);
    EXPECT_EQ(p.m_a.playing_take_id(), p.m_b.playing_take_id());
    EXPECT_EQ(p.m_a.playback_phase(), p.m_b.playback_phase());
    // Playback moves (the gesture was recorded, not a held value).
    const size_t half = p.m_out_a.m_x.size() / 2;
    EXPECT_GT(motion_test::max_sample_step(p.m_out_a.m_x, half, p.m_out_a.m_x.size()), 0.0);
}

// The recorder's clock advances by the whole oversized block: the playback
// phase after it equals the transport's (no divergence, no glide).
TEST(OversizedBlocks, RecorderPlaybackStaysOnTheTransport) {
    RecorderPair p;
    p.m_gesture.m_down = p.m_gesture.m_up = 0;  // no touch
    const MotionLane lane = motion_test::beats_lane(4.0, 96);
    p.m_a.load_lane(lane);
    p.m_b.load_lane(lane);
    for (int b = 0; b < 40; ++b) { p.block(playing(p.m_beat, b % 2 == 0 ? 3000 : k_bs)); }
    EXPECT_EQ(p.m_a.jump_glide_count(), 0u);
    const double want = std::fmod(p.m_beat, 4.0);
    EXPECT_NEAR(p.m_a.playback_phase(), want, 1e-9);
    EXPECT_EQ(p.m_a.playback_phase(), p.m_b.playback_phase());
}

// A jump flagged on an oversized block re-seeks and glides exactly once, as on
// its first prepared-size chunk.
TEST(OversizedBlocks, RecorderJumpOnAnOversizedBlockGlidesLikeChunks) {
    RecorderPair p;
    p.m_gesture.m_down = p.m_gesture.m_up = 0;
    const MotionLane lane = motion_test::beats_lane(4.0, 96);
    p.m_a.load_lane(lane);
    p.m_b.load_lane(lane);
    for (int b = 0; b < 20; ++b) { p.block(playing(p.m_beat, k_bs)); }
    TransportInfo jump = playing(p.m_beat + 1.3, 2 * k_big);
    jump.m_flags |= TransportInfo::k_jumped;
    jump.m_jump_delta_beats = 1.3;
    p.block(jump);
    for (int b = 0; b < 20; ++b) { p.block(playing(p.m_beat, k_bs)); }

    EXPECT_EQ(p.m_a.jump_glide_count(), 1u);
    EXPECT_EQ(p.m_b.jump_glide_count(), 1u);
    // A holds only the last chunk of the oversized block; compare the blocks after it.
    const size_t tail = 20 * k_bs;
    ASSERT_GE(p.m_out_a.m_x.size(), tail);
    const std::vector<float> a(p.m_out_a.m_x.end() - static_cast<std::ptrdiff_t>(tail),
                               p.m_out_a.m_x.end());
    const std::vector<float> ref(p.m_out_b.m_x.end() - static_cast<std::ptrdiff_t>(tail),
                                 p.m_out_b.m_x.end());
    EXPECT_EQ(a, ref);
}

// ── XYController ──────────────────────────────────────────────────────────────

namespace {

struct ControllerOutput {
    std::vector<float> m_x, m_y;
    std::vector<uint8_t> m_active;
    std::vector<XYLayer> m_layer;
    std::vector<uint32_t> m_cps;

    void append(const XYController& c) {
        const uint32_t n = c.num_samples();
        m_x.insert(m_x.end(), c.out_x(0), c.out_x(0) + n);
        m_y.insert(m_y.end(), c.out_y(0), c.out_y(0) + n);
        m_active.insert(m_active.end(), c.out_active(0), c.out_active(0) + n);
        m_layer.insert(m_layer.end(), c.out_layer(0), c.out_layer(0) + n);
        const auto cps = c.change_points(0);
        m_cps.insert(m_cps.end(), cps.begin(), cps.end());
    }
};

struct ControllerRig {
    ControllerRig() : m_matrix(m_backend), m_controller(m_matrix, make_config()) {
        m_matrix.prepare(k_sr, k_bs);
    }
    static XYControllerConfig make_config() {
        XYControllerConfig c;
        c.m_id = "pad";
        c.m_recorder.m_max_points = 8192;
        return c;
    }
    FakeBackend m_backend;
    ModulationMatrix m_matrix;
    XYController m_controller;
};

}  // namespace

// Touch, recording, latch, playback and UI frames through process_block():
// oversized host blocks give the same outputs, frames and trail as prepared-size ones.
TEST(OversizedBlocks, ControllerMatchesPreparedChunks) {
    ControllerRig a, b;
    XYController& ca = a.m_controller;
    XYController& cb = b.m_controller;
    ControllerOutput out_a, out_b;
    SimTransport transport;

    auto block = [&](uint32_t n) {
        const TransportInfo t = transport.next(n);
        ca.process_block(t);
        out_a.append(ca);
        const auto chunks = chunks_of(t);
        for (size_t k = 0; k < chunks.size(); ++k) {
            cb.process_block(chunks[k]);
            if (k + 1 == chunks.size()) { out_b.append(cb); }  // A holds the last chunk
        }
    };
    auto both = [&](auto&& f) {
        f(ca);
        f(cb);
    };

    both([](XYController& c) { ASSERT_TRUE(c.recorder().arm(LoopLength::Bars1)); });
    both([](XYController& c) { ASSERT_TRUE(c.touch(1, 0.3f, 0.6f)); });
    for (int i = 0; i < 120; ++i) {
        if (i % 7 == 0) {
            const float x = 0.1f + (0.007f * static_cast<float>(i));
            both([&](XYController& c) { ASSERT_TRUE(c.touch(1, x, 1.0f - x)); });
        }
        if (i == 40) {
            both([](XYController& c) { c.set_latch(true); });
        }
        if (i == 60) {
            both([](XYController& c) {
                ASSERT_TRUE(c.release(1));
                c.set_latch(false);
            });
        }
        if (i % 20 == 0) {
            both([](XYController& c) { c.service(); });
        }
        block(i % 2 == 0 ? k_big : 1500);
    }

    ASSERT_EQ(out_a.m_x.size(), out_b.m_x.size());
    EXPECT_EQ(out_a.m_x, out_b.m_x);
    EXPECT_EQ(out_a.m_y, out_b.m_y);
    EXPECT_EQ(out_a.m_active, out_b.m_active);
    EXPECT_EQ(out_a.m_layer, out_b.m_layer);
    EXPECT_EQ(out_a.m_cps, out_b.m_cps);
    EXPECT_EQ(ca.recorder().state(), MotionState::Playing);
    EXPECT_EQ(ca.recorder().playing_take_id(), cb.recorder().playing_take_id());
    EXPECT_EQ(ca.recorder().playback_phase(), cb.recorder().playback_phase());

    XYFrame fa, fb;
    ASSERT_TRUE(ca.read_frame(fa));
    ASSERT_TRUE(cb.read_frame(fb));
    EXPECT_EQ(fa.m_sample_time, fb.m_sample_time);
    EXPECT_EQ(fa.m_sample_time, 60u * (k_big + 1500u));
    EXPECT_EQ(fa.m_voices[0].m_x, fb.m_voices[0].m_x);
    EXPECT_EQ(fa.m_voices[0].m_motion_state, MotionState::Playing);

    std::array<XYPathPoint, k_xy_trail_capacity> ta{}, tb{};
    const size_t na = ca.drain_trail(ta);
    const size_t nb = cb.drain_trail(tb);
    ASSERT_EQ(na, nb);
    for (size_t i = 0; i < na; ++i) {
        EXPECT_EQ(ta[i].m_x, tb[i].m_x) << i;
        EXPECT_EQ(ta[i].m_progress, tb[i].m_progress) << i;
    }
}

// ── ModulationMatrix ──────────────────────────────────────────────────────────

// The matrix's buffers hold the prepared maximum: a larger num_samples is a
// contract violation (assert), clamped in a release build. Death tests need
// fork(), which googletest does not support on iOS / Android.
#if GTEST_HAS_DEATH_TEST
TEST(OversizedBlocksDeathTest, MatrixRejectsBlocksAboveThePreparedSize) {
    FakeBackend backend;
    backend.add("p", 0.0f);
    ModulationMatrix matrix(backend);
    matrix.prepare(k_sr, k_bs);
    matrix.process(k_bs);
    // Under RTSan the assert's own message write is reported first (the matrix
    // runs as a non-blocking function), so either report marks the violation.
    // googletest's simple regex (Windows) has no alternation; RTSan is
    // clang-only and never runs there.
#if GTEST_USES_SIMPLE_RE
    EXPECT_DEBUG_DEATH(matrix.process(k_bs + 1), "exceeds the prepared samples_per_block");
#else
    EXPECT_DEBUG_DEATH(matrix.process(k_bs + 1),
                       "exceeds the prepared samples_per_block|RealtimeSanitizer");
#endif
}
#endif  // GTEST_HAS_DEATH_TEST

// ── RTSan ─────────────────────────────────────────────────────────────────────

namespace {

float rt_controller_block(XYController& c, const TransportInfo& t) TANH_NONBLOCKING_FUNCTION {
    c.process_block(t);
    return c.out_x(0)[c.num_samples() - 1] + static_cast<float>(c.change_points(0).size());
}

float rt_recorder_block(MotionRecorder& r,
                        const TransportInfo& t,
                        const XYPadStream& in) TANH_NONBLOCKING_FUNCTION {
    r.process(t, in, t.m_num_samples);
    return r.out_x()[r.num_samples() - 1];
}

}  // namespace

// Under -DTANH_WITH_RTSAN=ON this aborts if chunking an oversized block
// allocates, locks or makes a syscall (recording, a take finishing, playback).
TEST(OversizedBlocksRtsan, ChunkedBlocksAreRealtimeSafe) {
    ControllerRig rig;
    XYController& c = rig.m_controller;
    SimTransport transport;
    ASSERT_TRUE(c.recorder().arm(LoopLength::Bars1));
    ASSERT_TRUE(c.touch(1, 0.3f, 0.6f));
    float acc = 0.0f;
    for (int i = 0; i < 120; ++i) {
        if (i == 30) { ASSERT_TRUE(c.release(1)); }
        acc += rt_controller_block(c, transport.next(4 * k_bs + 17));
        if (i % 20 == 0) { c.service(); }
    }
    EXPECT_EQ(c.recorder().state(), MotionState::Playing);

    MotionRecorder r;
    r.prepare(k_sr, k_bs);
    ASSERT_TRUE(r.arm(LoopLength::Free));
    const Gesture g{.m_down = 10, .m_up = 20000};
    double beat = 0.0;
    for (uint64_t s = 0; s < 60000; s += k_big) {
        const Stream in(g, s, k_big);
        const TransportInfo t = playing(beat, k_big);
        acc += rt_recorder_block(r, t, in.view());
        beat = t.beat_end();
        if (s % (10 * k_big) == 0) { r.service(); }
    }
    EXPECT_NE(acc, 0.0f);
}
