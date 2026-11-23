#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/modulation/detail/MotionClock.h>

TEST(MotionClock, FollowsThePlayingTransportAndFreeRunsWhenStopped) {
    using thl::dsp::transport::TransportInfo;
    thl::modulation::detail::MotionClock clock;
    TransportInfo t;
    t.m_flags = TransportInfo::k_is_playing;
    t.m_bpm = 120.0;
    t.m_beat_position = 8.0;
    t.m_beats_per_sample = 120.0 / (60.0 * 48000.0);
    auto c = clock.begin(t, 48000.0, false);
    EXPECT_DOUBLE_EQ(c.m_beat, 8.0);
    EXPECT_DOUBLE_EQ(c.m_slope, t.m_beats_per_sample);
    clock.end(c, 480);

    // Stopped: the beat continues from the last block's end, at the last tempo.
    t.m_flags = 0;
    t.m_beat_position = 0.0;
    c = clock.begin(t, 48000.0, false);
    EXPECT_DOUBLE_EQ(c.m_beat, 8.0 + (480 * t.m_beats_per_sample));
    EXPECT_GT(c.m_slope, 0.0);
    EXPECT_GT(c.m_rec_slope, 0.0);
    clock.end(c, 480);

    // Held: the playback beat stands still, the recording clock keeps running.
    c = clock.begin(t, 48000.0, true);
    EXPECT_DOUBLE_EQ(c.m_slope, 0.0);
    EXPECT_GT(c.m_rec_slope, 0.0);

    // A timeline reset restarts from the transport's beat.
    t.m_flags = TransportInfo::k_timeline_reset;
    t.m_beat_position = 2.0;
    c = clock.begin(t, 48000.0, false);
    EXPECT_DOUBLE_EQ(c.m_beat, 2.0);
    EXPECT_TRUE(c.m_timeline_reset);
}
