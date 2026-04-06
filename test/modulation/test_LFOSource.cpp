#include <gtest/gtest.h>

#include "TestHelpers.h"

using namespace thl::modulation;

TEST(LFOSource, SineOutputRange) {
    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_waveform = LFOWaveform::Sine;
    lfo.prepare(k_sample_rate, k_block_size);

    lfo.process(k_block_size);

    const auto& output = lfo.get_output_buffer();
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_GE(output[i], -1.0f);
        EXPECT_LE(output[i], 1.0f);
    }
}

TEST(LFOSource, TriangleOutputRange) {
    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_waveform = LFOWaveform::Triangle;
    lfo.prepare(k_sample_rate, k_block_size);

    lfo.process(k_block_size);

    const auto& output = lfo.get_output_buffer();
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_GE(output[i], -1.0f);
        EXPECT_LE(output[i], 1.0f);
    }
}

TEST(LFOSource, SawOutputRange) {
    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_waveform = LFOWaveform::Saw;
    lfo.prepare(k_sample_rate, k_block_size);

    lfo.process(k_block_size);

    const auto& output = lfo.get_output_buffer();
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_GE(output[i], -1.0f);
        EXPECT_LE(output[i], 1.0f);
    }
}

TEST(LFOSource, SquareOutputValues) {
    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_waveform = LFOWaveform::Square;
    lfo.prepare(k_sample_rate, k_block_size);

    lfo.process(k_block_size);

    const auto& output = lfo.get_output_buffer();
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_TRUE(output[i] == 1.0f || output[i] == -1.0f);
    }
}

TEST(LFOSource, DecimationReducesChangePoints) {
    TestLFOSource lfo_fast;
    lfo_fast.m_frequency = 10.0f;
    lfo_fast.m_decimation = 1;
    lfo_fast.prepare(k_sample_rate, k_block_size);
    lfo_fast.process(k_block_size);

    TestLFOSource lfo_slow;
    lfo_slow.m_frequency = 10.0f;
    lfo_slow.m_decimation = 16;
    lfo_slow.prepare(k_sample_rate, k_block_size);
    lfo_slow.process(k_block_size);

    EXPECT_GT(lfo_fast.get_change_points().size(), lfo_slow.get_change_points().size());
}

TEST(LFOSource, ProcessSingleMatchesBulk) {
    TestLFOSource lfo_bulk;
    lfo_bulk.m_frequency = 5.0f;
    lfo_bulk.m_waveform = LFOWaveform::Sine;
    lfo_bulk.m_decimation = 1;
    lfo_bulk.prepare(k_sample_rate, k_block_size);
    lfo_bulk.process(k_block_size);

    TestLFOSource lfo_single;
    lfo_single.m_frequency = 5.0f;
    lfo_single.m_waveform = LFOWaveform::Sine;
    lfo_single.m_decimation = 1;
    lfo_single.prepare(k_sample_rate, k_block_size);

    std::vector<float> single_output(k_block_size);
    for (size_t i = 0; i < k_block_size; ++i) {
        lfo_single.process_single(&single_output[i], static_cast<uint32_t>(i));
    }

    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(lfo_bulk.get_output_buffer()[i], single_output[i])
            << "Mismatch at sample " << i;
    }
}
