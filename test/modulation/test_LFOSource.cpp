#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "TestHelpers.h"

using namespace thl::modulation;

TEST(LFOSource, SineOutputRange) {
    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_waveform = LFOWaveform::Sine;
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);

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
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);

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
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);

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
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);

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
    lfo_fast.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo_fast.process(k_block_size);

    TestLFOSource lfo_slow;
    lfo_slow.m_frequency = 10.0f;
    lfo_slow.m_decimation = 16;
    lfo_slow.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo_slow.process(k_block_size);

    EXPECT_GT(lfo_fast.get_change_points().size(), lfo_slow.get_change_points().size());
}

TEST(LFOSource, ProcessSingleMatchesBulk) {
    TestLFOSource lfo_bulk;
    lfo_bulk.m_frequency = 5.0f;
    lfo_bulk.m_waveform = LFOWaveform::Sine;
    lfo_bulk.m_decimation = 1;
    lfo_bulk.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo_bulk.process(k_block_size);

    TestLFOSource lfo_single;
    lfo_single.m_frequency = 5.0f;
    lfo_single.m_waveform = LFOWaveform::Sine;
    lfo_single.m_decimation = 1;
    lfo_single.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);

    for (size_t i = 0; i < k_block_size; ++i) { lfo_single.process(1, i); }

    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(lfo_bulk.get_output_buffer()[i], lfo_single.get_output_buffer()[i])
            << "Mismatch at sample " << i;
    }
}

TEST(LFOSource, SawDownDescends) {
    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;
    lfo.m_waveform = LFOWaveform::SawDown;
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo.process(k_block_size);

    const auto& output = lfo.get_output_buffer();
    // First sample near +1, decreasing over the block (cycle is 1 Hz at 48 kHz
    // → 48k samples per cycle, so within a 512-sample block we descend a
    // small fraction).
    EXPECT_GT(output[0], output[k_block_size - 1]);
    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_GE(output[i], -1.0f);
        EXPECT_LE(output[i], 1.0f);
    }
}

TEST(LFOSource, PulseWidthAffectsDuty) {
    TestLFOSource lfo;
    lfo.m_frequency = 1000.0f;  // ~10 cycles per 512-sample block at 48 kHz.
    lfo.m_waveform = LFOWaveform::Square;
    lfo.m_pulse_width = 0.25f;
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    // Run several blocks so the average converges.
    for (int b = 0; b < 16; ++b) { lfo.process(k_block_size); }

    int high = 0;
    int total = 0;
    for (size_t i = 0; i < k_block_size; ++i) {
        if (lfo.get_output_buffer()[i] > 0.0f) { ++high; }
        ++total;
    }
    const float ratio = static_cast<float>(high) / static_cast<float>(total);
    EXPECT_NEAR(ratio, 0.25f, 0.05f);
}

TEST(LFOSource, SampleAndHoldStaysConstantBetweenWraps) {
    TestLFOSource lfo;
    lfo.m_frequency = 1.0f;  // ~48000 samples per cycle, way longer than block.
    lfo.m_waveform = LFOWaveform::SampleAndHold;
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo.process(k_block_size);

    // Output should be constant across the block (no phase wrap within 512 samples).
    const float first = lfo.get_output_buffer()[0];
    for (size_t i = 1; i < k_block_size; ++i) {
        EXPECT_FLOAT_EQ(lfo.get_output_buffer()[i], first);
    }
    EXPECT_GE(first, -1.0f);
    EXPECT_LE(first, 1.0f);
}

TEST(LFOSource, PhaseOffsetShiftsOutput) {
    TestLFOSource lfo_a;
    lfo_a.m_frequency = 10.0f;
    lfo_a.m_waveform = LFOWaveform::Sine;
    lfo_a.m_phase_offset = 0.0f;
    lfo_a.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo_a.process(k_block_size);

    TestLFOSource lfo_b;
    lfo_b.m_frequency = 10.0f;
    lfo_b.m_waveform = LFOWaveform::Sine;
    lfo_b.m_phase_offset = 0.25f;  // 90° → cosine.
    lfo_b.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo_b.process(k_block_size);

    // Sine at phase 0 = 0, sine at phase 0.25 = +1.
    EXPECT_NEAR(lfo_a.get_output_buffer()[0], 0.0f, 0.01f);
    EXPECT_NEAR(lfo_b.get_output_buffer()[0], 1.0f, 0.01f);
}

TEST(LFOSource, BiasShiftsDC) {
    TestLFOSource lfo;
    lfo.m_frequency = 200.0f;  // Many cycles per block — mean approaches 0.
    lfo.m_waveform = LFOWaveform::Sine;
    lfo.m_bias = 0.3f;
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo.process(k_block_size);

    float sum = 0.0f;
    for (size_t i = 0; i < k_block_size; ++i) { sum += lfo.get_output_buffer()[i]; }
    const float mean = sum / static_cast<float>(k_block_size);
    EXPECT_NEAR(mean, 0.3f, 0.05f);
}

TEST(LFOSource, DepthScalesOutput) {
    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_waveform = LFOWaveform::Saw;
    lfo.m_depth = 0.0f;
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo.process(k_block_size);

    for (size_t i = 0; i < k_block_size; ++i) { EXPECT_FLOAT_EQ(lfo.get_output_buffer()[i], 0.0f); }
}

TEST(LFOSource, UnipolarMapsZeroToOne) {
    TestLFOSource lfo;
    lfo.m_frequency = 10.0f;
    lfo.m_waveform = LFOWaveform::Sine;
    lfo.m_polarity = LFOPolarity::Unipolar;
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo.process(k_block_size);

    for (size_t i = 0; i < k_block_size; ++i) {
        EXPECT_GE(lfo.get_output_buffer()[i], 0.0f);
        EXPECT_LE(lfo.get_output_buffer()[i], 1.0f);
    }
}

TEST(LFOSource, FadeInRampsLinearly) {
    TestLFOSource lfo;
    lfo.m_frequency = 200.0f;  // Hold output near peak quickly via square.
    lfo.m_waveform = LFOWaveform::Square;
    lfo.m_fade_in = 0.01f;  // 10 ms, ~480 samples at 48 kHz.
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo.reset_fade_in();
    lfo.process(k_block_size);

    // First sample should be near zero (fade just starting).
    EXPECT_NEAR(std::abs(lfo.get_output_buffer()[0]), 0.0f, 0.05f);
    // Last sample of block should have grown (block is roughly the fade-in length).
    EXPECT_GT(std::abs(lfo.get_output_buffer()[k_block_size - 1]),
              std::abs(lfo.get_output_buffer()[0]));
}

TEST(LFOSource, FadeInCompletes) {
    TestLFOSource lfo;
    lfo.m_frequency = 200.0f;
    lfo.m_waveform = LFOWaveform::Square;
    lfo.m_fade_in = 0.001f;  // 1 ms, finishes well within a block.
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo.reset_fade_in();
    lfo.process(k_block_size);

    EXPECT_FLOAT_EQ(lfo.current_fade_in(), 1.0f);
}

TEST(LFOSource, SmoothSlewsTowardTarget) {
    TestLFOSource lfo_fast;
    lfo_fast.m_frequency = 200.0f;
    lfo_fast.m_waveform = LFOWaveform::Square;
    lfo_fast.m_smooth = 0.0f;
    lfo_fast.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo_fast.process(k_block_size);

    TestLFOSource lfo_smooth;
    lfo_smooth.m_frequency = 200.0f;
    lfo_smooth.m_waveform = LFOWaveform::Square;
    lfo_smooth.m_smooth = 0.5f;  // ~0.5 s tau, much longer than the block.
    lfo_smooth.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);
    lfo_smooth.process(k_block_size);

    // Smoothed signal has smaller peak deviation than unsmoothed (which sits
    // at ±1.0 for square).
    float smooth_peak = 0.0f;
    float fast_peak = 0.0f;
    for (size_t i = 0; i < k_block_size; ++i) {
        smooth_peak = std::max(smooth_peak, std::abs(lfo_smooth.get_output_buffer()[i]));
        fast_peak = std::max(fast_peak, std::abs(lfo_fast.get_output_buffer()[i]));
    }
    EXPECT_LT(smooth_peak, fast_peak);
}

TEST(LFOSource, CurrentPhaseAdvances) {
    TestLFOSource lfo;
    lfo.m_frequency = 100.0f;  // ~480 samples per cycle.
    lfo.m_waveform = LFOWaveform::Sine;
    lfo.prepare(k_sample_rate, k_block_size, /*voice_count=*/1);

    EXPECT_FLOAT_EQ(lfo.current_phase(), 0.0f);
    lfo.process(k_block_size);
    EXPECT_GT(lfo.current_phase(), 0.0f);
}
