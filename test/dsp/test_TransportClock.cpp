#include <gtest/gtest.h>
#include <tanh/dsp/transport/TransportClock.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <cstdint>

using thl::dsp::transport::Division;
using thl::dsp::transport::TransportInfo;

TEST(TransportClockBase, GettersReadTheComputedBlock) {
    // A subclass only computes the block; the base keeps it and answers the getters.
    class Minimal final : public thl::dsp::transport::TransportClock {
    public:
        void set_bpm(double) override {}
        void set_time_signature(int, int) override {}
        void play() override {}
        void stop() override {}
        void set_position_beats(double) override {}
        bool m_playing = true;

    protected:
        void on_prepare(double) override {}
        [[nodiscard]] TransportInfo compute_block(uint32_t frames) override {
            TransportInfo info;
            info.m_flags = m_playing ? TransportInfo::k_is_playing : 0u;
            info.m_bpm = 90.0;
            info.m_beat_position = 2.0;
            info.m_beats_per_sample = 0.5;
            info.m_sig_num = 3;
            info.m_num_samples = frames;
            return info;
        }
    } clock;
    clock.prepare(48000.0);
    clock.begin_block(64);
    EXPECT_TRUE(clock.is_playing());
    EXPECT_DOUBLE_EQ(clock.bpm(), 90.0);
    EXPECT_DOUBLE_EQ(clock.beat_at_sample(2), 3.0);
    EXPECT_EQ(clock.sig_num(), 3);
    EXPECT_EQ(clock.block_info().m_num_samples, 64u);
    clock.end_block();
    EXPECT_EQ(clock.sample_position(), 64u);  // counts while playing

    clock.m_playing = false;
    clock.begin_block(64);
    EXPECT_FALSE(clock.division_in_block(Division::Sixteenth));  // gated on playing
    clock.end_block();
    EXPECT_EQ(clock.sample_position(), 64u);

    clock.prepare(48000.0);
    EXPECT_EQ(clock.sample_position(), 0u);
}
