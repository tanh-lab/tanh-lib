#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/MotionRecorder.h>
#include <tanh/modulation/XYController.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "XYControllerTestHelpers.h"

using namespace thl::modulation;
using thl::dsp::transport::TransportInfo;
using xy_test::FakeBackend;
using xy_test::k_sr;
using xy_test::SimTransport;

namespace {

constexpr uint32_t k_bs = 256;    // prepared maximum
constexpr uint32_t k_big = 1000;  // host block: 256 + 256 + 256 + 232

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

struct ControllerOutput {
    std::vector<float> m_x;
    std::vector<float> m_y;
    std::vector<uint8_t> m_active;
    std::vector<XYLayer> m_layer;
    std::vector<uint32_t> m_change_points;

    void append(const XYController& c) {
        const uint32_t n = c.num_samples();
        m_x.insert(m_x.end(), c.out_x(0), c.out_x(0) + n);
        m_y.insert(m_y.end(), c.out_y(0), c.out_y(0) + n);
        m_active.insert(m_active.end(), c.out_active(0), c.out_active(0) + n);
        m_layer.insert(m_layer.end(), c.out_layer(0), c.out_layer(0) + n);
        const auto cps = c.change_points(0);
        m_change_points.insert(m_change_points.end(), cps.begin(), cps.end());
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
    ControllerRig a;
    ControllerRig b;
    XYController& ca = a.m_controller;
    XYController& cb = b.m_controller;
    ControllerOutput out_a;
    ControllerOutput out_b;
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
    EXPECT_EQ(out_a.m_change_points, out_b.m_change_points);
    EXPECT_EQ(ca.recorder().playback_phase(), cb.recorder().playback_phase());

    XYFrame fa;
    XYFrame fb;
    ASSERT_TRUE(ca.read_frame(fa));
    ASSERT_TRUE(cb.read_frame(fb));
    EXPECT_EQ(fa.m_sample_time, fb.m_sample_time);
    EXPECT_EQ(fa.m_sample_time, 60u * (k_big + 1500u));
    EXPECT_EQ(fa.m_voices[0].m_x, fb.m_voices[0].m_x);
    EXPECT_EQ(fa.m_voices[0].m_motion.m_state, MotionState::Playing);
    EXPECT_EQ(fa.m_voices[0].m_motion.m_take_id, fb.m_voices[0].m_motion.m_take_id);

    std::array<XYPathPoint, k_xy_trail_capacity> ta{};
    std::array<XYPathPoint, k_xy_trail_capacity> tb{};
    const size_t na = ca.drain_trail(ta);
    const size_t nb = cb.drain_trail(tb);
    ASSERT_EQ(na, nb);
    for (size_t i = 0; i < na; ++i) {
        EXPECT_EQ(ta[i].m_x, tb[i].m_x) << i;
        EXPECT_EQ(ta[i].m_progress, tb[i].m_progress) << i;
    }
}

// The matrix's buffers hold the prepared maximum: a larger num_samples is a
// contract violation (assert), clamped in a release build.
TEST(OversizedBlocksDeathTest, MatrixRejectsBlocksAboveThePreparedSize) {
    FakeBackend backend;
    backend.add("p", 0.0f);
    ModulationMatrix matrix(backend);
    matrix.prepare(k_sr, k_bs);
    matrix.process(k_bs);
    // Under RTSan the assert's own message write is reported first (the matrix
    // runs as a non-blocking function), so either report marks the violation.
    EXPECT_DEBUG_DEATH(matrix.process(k_bs + 1),
                       "exceeds the prepared samples_per_block|RealtimeSanitizer");
}
