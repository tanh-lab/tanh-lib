#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <cstdint>

using thl::dsp::transport::Division;
using thl::dsp::transport::TransportInfo;

namespace {

TransportInfo raw(double beat, bool playing, double bpm = 120.0) {
    TransportInfo info;
    info.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position;
    if (playing) { info.m_flags |= TransportInfo::k_is_playing; }
    info.m_bpm = bpm;
    info.m_beat_position = beat;
    return info;
}

}  // namespace

TEST(TransportInfo, PhaseIsNegativeSafe) {
    TransportInfo info;
    info.m_beat_position = -0.5;
    EXPECT_DOUBLE_EQ(info.phase(4.0), 3.5);
    info.m_beat_position = -4.0;
    EXPECT_DOUBLE_EQ(info.phase(4.0), 0.0);
    info.m_beat_position = 9.25;
    EXPECT_DOUBLE_EQ(info.phase(4.0), 1.25);
    info.m_beat_position = -1e-18;  // rounds to unit → must wrap to 0
    EXPECT_GE(info.phase(4.0), 0.0);
    EXPECT_LT(info.phase(4.0), 4.0);
    EXPECT_DOUBLE_EQ(info.phase(0.0), 0.0);
}

TEST(TransportInfo, BeatAtAndEnd) {
    TransportInfo info;
    info.m_beat_position = 2.0;
    info.m_beats_per_sample = 0.01;
    info.m_num_samples = 100;
    EXPECT_DOUBLE_EQ(info.beat_at(50), 2.5);
    EXPECT_DOUBLE_EQ(info.beat_end(), 3.0);
    EXPECT_DOUBLE_EQ(info.phase(1.0, 50), 0.5);
}

TEST(TransportInfo, SubBlockKeepsDiscontinuitiesOnTheFirstChunkOnly) {
    TransportInfo info = raw(2.0, true);
    info.m_flags |= TransportInfo::k_jumped | TransportInfo::k_tempo_changed;
    info.m_beats_per_sample = 0.01;
    info.m_jump_delta_beats = 0.5;
    info.m_num_samples = 300;

    const TransportInfo first = info.sub_block(0, 128);
    EXPECT_EQ(first.m_flags, info.m_flags);
    EXPECT_DOUBLE_EQ(first.m_beat_position, 2.0);
    EXPECT_EQ(first.m_num_samples, 128u);
    EXPECT_DOUBLE_EQ(first.m_jump_delta_beats, 0.5);

    const TransportInfo last = info.sub_block(256, 44);
    EXPECT_EQ(last.discontinuities(), 0u);
    EXPECT_TRUE(last.is_playing());
    EXPECT_TRUE(last.has(TransportInfo::k_has_tempo));
    EXPECT_DOUBLE_EQ(last.m_beat_position, info.beat_at(256));
    EXPECT_DOUBLE_EQ(last.m_beats_per_sample, 0.01);
    EXPECT_DOUBLE_EQ(last.m_jump_delta_beats, 0.0);
    EXPECT_EQ(last.m_num_samples, 44u);
    EXPECT_NEAR(last.beat_end(), info.beat_end(), 1e-12);
}

TEST(TransportInfo, DivisionBoundaryIsHalfOpenAndNegativeSafe) {
    using thl::dsp::transport::division_boundary_in;
    EXPECT_TRUE(division_boundary_in(0.0, 0.1, Division::Beat, 4, 4));   // boundary at start
    EXPECT_FALSE(division_boundary_in(0.9, 1.0, Division::Beat, 4, 4));  // at end → next block
    EXPECT_TRUE(division_boundary_in(-0.1, 0.1, Division::Bar, 4, 4));   // count-in crosses 0
    EXPECT_TRUE(division_boundary_in(-4.05, -3.95, Division::Bar, 4, 4));
    EXPECT_FALSE(division_boundary_in(1.0, 1.0, Division::Beat, 4, 4));  // empty
    EXPECT_FALSE(division_boundary_in(2.0, 1.0, Division::Beat, 4, 4));  // reversed
    EXPECT_TRUE(division_boundary_in(3.4, 3.6, Division::Bar, 7, 8));    // 7/8 bar = 3.5
}
