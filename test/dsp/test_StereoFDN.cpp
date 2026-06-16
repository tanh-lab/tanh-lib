#include <gtest/gtest.h>
#include <tanh/dsp/audio/AudioBuffer.h>
#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/fx/StereoFDN.h>

#include <algorithm>

namespace {

constexpr double k_sample_rate = 48000.0;

using thl::dsp::audio::AudioBuffer;
using thl::dsp::audio::AudioBufferView;
using thl::dsp::fx::StereoFDN;

void prepare_for_unit_delay_test(StereoFDN& fdn) {
    fdn.prepare(k_sample_rate, 16, 2);
    fdn.set_feedback(0.5f);
    fdn.set_damping(0.0f);
    fdn.set_cross_feedback(0.0f);
    fdn.set_input_pan(0.0f);
    fdn.set_dry(0.0f);
    fdn.set_wet(1.0f);
    fdn.set_linear_smoothing_samples(0);
}

}  // namespace

TEST(StereoFDN, OneDelayPerChannelProducesExpectedIndependentEchoes) {
    StereoFDN fdn;
    prepare_for_unit_delay_test(fdn);
    fdn.set_delay_samples(3, 5);

    AudioBuffer buffer(2, 12, k_sample_rate);
    buffer.get_write_pointer(0)[0] = 1.0f;
    buffer.get_write_pointer(1)[1] = 1.0f;

    fdn.process(AudioBufferView(buffer));

    const float* left = buffer.get_read_pointer(0);
    const float* right = buffer.get_read_pointer(1);

    EXPECT_FLOAT_EQ(left[0], 0.0f);
    EXPECT_FLOAT_EQ(left[3], 1.0f);
    EXPECT_FLOAT_EQ(left[6], 0.5f);
    EXPECT_FLOAT_EQ(left[9], 0.25f);

    EXPECT_FLOAT_EQ(right[0], 0.0f);
    EXPECT_FLOAT_EQ(right[6], 1.0f);
    EXPECT_FLOAT_EQ(right[11], 0.5f);

    EXPECT_FLOAT_EQ(right[3], 0.0f);
    EXPECT_FLOAT_EQ(left[11], 0.0f);
}

TEST(StereoFDN, DelayStateContinuesAcrossBlocks) {
    StereoFDN fdn;
    prepare_for_unit_delay_test(fdn);
    fdn.set_delay_samples(3, 3);

    AudioBuffer first_block(2, 4, k_sample_rate);
    first_block.get_write_pointer(0)[0] = 1.0f;

    fdn.process(AudioBufferView(first_block));

    EXPECT_FLOAT_EQ(first_block.get_read_pointer(0)[3], 1.0f);

    AudioBuffer second_block(2, 4, k_sample_rate);
    fdn.process(AudioBufferView(second_block));

    EXPECT_FLOAT_EQ(second_block.get_read_pointer(0)[2], 0.5f);
    EXPECT_FLOAT_EQ(second_block.get_read_pointer(1)[2], 0.0f);
}

TEST(StereoFDN, DefaultsToLongCollapsedDelayLayout) {
    StereoFDN fdn(5);
    fdn.prepare(k_sample_rate, 16, 2);

    EXPECT_FLOAT_EQ(fdn.base_time_ms(), 500.0f);
    EXPECT_FLOAT_EQ(fdn.delay_spread(), 0.0f);
    for (size_t channel = 0; channel < StereoFDN::k_num_audio_channels; ++channel) {
        for (size_t line = 0; line < fdn.delay_lines_per_channel(); ++line) {
            EXPECT_EQ(fdn.delay_samples(channel, line), 24000u);
        }
    }
}

TEST(StereoFDN, CrossfadesDelayChangesAfterProcessingStarts) {
    StereoFDN fdn;
    fdn.prepare(k_sample_rate, 16, 2);
    fdn.set_feedback(0.0f);
    fdn.set_cross_feedback(0.0f);
    fdn.set_dry(0.0f);
    fdn.set_wet(1.0f);
    fdn.set_linear_smoothing_samples(0);
    fdn.set_crossfade_samples(4);
    fdn.set_delay_samples(2, 2);

    AudioBuffer first_block(2, 8, k_sample_rate);
    for (size_t i = 0; i < first_block.get_num_frames(); ++i) {
        first_block.get_write_pointer(0)[i] = static_cast<float>(i + 1);
    }
    fdn.process(AudioBufferView(first_block));

    fdn.set_delay_sample(StereoFDN::k_left, 0, 4);

    AudioBuffer second_block(2, 6, k_sample_rate);
    for (size_t i = 0; i < second_block.get_num_frames(); ++i) {
        second_block.get_write_pointer(0)[i] = static_cast<float>(i + 9);
    }
    fdn.process(AudioBufferView(second_block));

    const float* left = second_block.get_read_pointer(0);

    EXPECT_FLOAT_EQ(left[0], 7.0f);
    EXPECT_FLOAT_EQ(left[1], 7.5f);
    EXPECT_FLOAT_EQ(left[2], 8.0f);
    EXPECT_FLOAT_EQ(left[3], 8.5f);
    EXPECT_FLOAT_EQ(left[4], 9.0f);
}

TEST(StereoFDN, SupportsMultipleDelayLinesPerChannel) {
    StereoFDN fdn(2);
    fdn.prepare(k_sample_rate, 16, 2);
    fdn.set_feedback(0.0f);
    fdn.set_cross_feedback(0.0f);
    fdn.set_dry(0.0f);
    fdn.set_wet(1.0f);
    fdn.set_linear_smoothing_samples(0);
    fdn.set_delay_sample(StereoFDN::k_left, 0, 2);
    fdn.set_delay_sample(StereoFDN::k_left, 1, 4);
    fdn.set_delay_sample(StereoFDN::k_right, 0, 3);
    fdn.set_delay_sample(StereoFDN::k_right, 1, 5);

    AudioBuffer buffer(2, 8, k_sample_rate);
    buffer.get_write_pointer(0)[0] = 1.0f;
    buffer.get_write_pointer(1)[1] = 1.0f;

    fdn.process(AudioBufferView(buffer));

    EXPECT_EQ(fdn.delay_lines_per_channel(), 2u);
    EXPECT_EQ(fdn.total_delay_lines(), 4u);
    EXPECT_EQ(fdn.delay_samples(StereoFDN::k_left, 1), 4u);

    const float* left = buffer.get_read_pointer(0);
    const float* right = buffer.get_read_pointer(1);

    EXPECT_FLOAT_EQ(left[2], 0.5f);
    EXPECT_FLOAT_EQ(left[4], 0.5f);
    EXPECT_FLOAT_EQ(right[4], 0.5f);
    EXPECT_FLOAT_EQ(right[6], 0.5f);
}

TEST(StereoFDN, DelaySpreadZeroCollapsesLinesToBaseTime) {
    StereoFDN fdn(5);
    fdn.prepare(k_sample_rate, 16, 2);
    fdn.set_linear_smoothing_samples(0);
    fdn.set_base_time_ms(100.0f);
    fdn.set_delay_spread(0.0f);

    for (size_t channel = 0; channel < StereoFDN::k_num_audio_channels; ++channel) {
        for (size_t line = 0; line < fdn.delay_lines_per_channel(); ++line) {
            EXPECT_EQ(fdn.delay_samples(channel, line), 4800u);
        }
    }
}

TEST(StereoFDN, DelaySpreadFansOutAroundBaseTime) {
    StereoFDN fdn(5);
    fdn.prepare(k_sample_rate, 16, 2);
    fdn.set_linear_smoothing_samples(0);
    fdn.set_base_time_ms(100.0f);
    fdn.set_delay_spread(1.0f);

    size_t min_delay = fdn.delay_samples(0, 0);
    size_t max_delay = fdn.delay_samples(0, 0);
    for (size_t channel = 0; channel < StereoFDN::k_num_audio_channels; ++channel) {
        for (size_t line = 0; line < fdn.delay_lines_per_channel(); ++line) {
            min_delay = std::min(min_delay, fdn.delay_samples(channel, line));
            max_delay = std::max(max_delay, fdn.delay_samples(channel, line));
        }
    }

    EXPECT_LT(min_delay, 4800u);
    EXPECT_GT(max_delay, 4800u);
}

TEST(StereoFDN, DampingDarkensFeedbackPath) {
    auto render_second_echo = [](float damping) {
        StereoFDN fdn;
        prepare_for_unit_delay_test(fdn);
        fdn.set_delay_samples(2, 2);
        fdn.set_damping(damping);

        AudioBuffer buffer(2, 6, k_sample_rate);
        buffer.get_write_pointer(0)[0] = 1.0f;
        fdn.process(AudioBufferView(buffer));
        return buffer.get_read_pointer(0)[4];
    };

    const float bright = render_second_echo(0.0f);
    const float dark = render_second_echo(1.0f);

    EXPECT_FLOAT_EQ(bright, 0.5f);
    EXPECT_GT(dark, 0.0f);
    EXPECT_LT(dark, bright * 0.25f);
}

TEST(StereoFDN, MatrixKindChangesPerLineFeedbackRouting) {
    auto render_sample_two = [](StereoFDN::MatrixKind kind) {
        StereoFDN fdn(2);
        fdn.prepare(k_sample_rate, 16, 2);
        fdn.set_feedback(0.5f);
        fdn.set_damping(0.0f);
        fdn.set_cross_feedback(0.0f);
        fdn.set_dry(0.0f);
        fdn.set_wet(1.0f);
        fdn.set_linear_smoothing_samples(0);
        fdn.set_matrix_kind(kind);
        fdn.set_delay_sample(StereoFDN::k_left, 0, 1);
        fdn.set_delay_sample(StereoFDN::k_left, 1, 3);
        fdn.set_delay_sample(StereoFDN::k_right, 0, 1);
        fdn.set_delay_sample(StereoFDN::k_right, 1, 3);

        AudioBuffer buffer(2, 6, k_sample_rate);
        buffer.get_write_pointer(0)[0] = 1.0f;
        fdn.process(AudioBufferView(buffer));
        return buffer.get_read_pointer(0)[2];
    };

    EXPECT_GT(render_sample_two(StereoFDN::MatrixKind::Diagonal), 0.2f);
    EXPECT_FLOAT_EQ(render_sample_two(StereoFDN::MatrixKind::Circulant), 0.0f);
}

TEST(StereoFDN, StoresSelectedMatrixKind) {
    StereoFDN fdn(5);
    fdn.prepare(k_sample_rate, 16, 2);

    EXPECT_EQ(fdn.matrix_kind(), StereoFDN::MatrixKind::Diagonal);
    fdn.set_matrix_kind(StereoFDN::MatrixKind::Householder);
    EXPECT_EQ(fdn.matrix_kind(), StereoFDN::MatrixKind::Householder);
    fdn.set_matrix_kind(StereoFDN::MatrixKind::Circulant);
    EXPECT_EQ(fdn.matrix_kind(), StereoFDN::MatrixKind::Circulant);
    fdn.set_matrix_kind(StereoFDN::MatrixKind::Triangular);
    EXPECT_EQ(fdn.matrix_kind(), StereoFDN::MatrixKind::Triangular);
    fdn.set_matrix_kind(StereoFDN::MatrixKind::Hadamard);
    EXPECT_EQ(fdn.matrix_kind(), StereoFDN::MatrixKind::Hadamard);
    fdn.set_matrix_kind(StereoFDN::MatrixKind::Anderson);
    EXPECT_EQ(fdn.matrix_kind(), StereoFDN::MatrixKind::Anderson);
    fdn.set_matrix_kind(StereoFDN::MatrixKind::Diagonal);
    EXPECT_EQ(fdn.matrix_kind(), StereoFDN::MatrixKind::Diagonal);
    fdn.set_matrix_kind(StereoFDN::MatrixKind::RandomOrthogonal);
    EXPECT_EQ(fdn.matrix_kind(), StereoFDN::MatrixKind::RandomOrthogonal);
}

TEST(StereoFDN, MatrixKindChangeClearsCirculatingEnergy) {
    StereoFDN fdn;
    prepare_for_unit_delay_test(fdn);
    fdn.set_delay_samples(3, 3);

    AudioBuffer first_block(2, 4, k_sample_rate);
    first_block.get_write_pointer(0)[0] = 1.0f;
    fdn.process(AudioBufferView(first_block));

    EXPECT_FLOAT_EQ(first_block.get_read_pointer(0)[3], 1.0f);

    fdn.set_matrix_kind(StereoFDN::MatrixKind::Householder);

    AudioBuffer second_block(2, 4, k_sample_rate);
    fdn.process(AudioBufferView(second_block));

    EXPECT_FLOAT_EQ(second_block.get_read_pointer(0)[2], 0.0f);
    EXPECT_FLOAT_EQ(second_block.get_read_pointer(1)[2], 0.0f);
}

TEST(StereoFDN, SmoothsDelayTargetsWithFractionalReads) {
    StereoFDN fdn;
    fdn.prepare(k_sample_rate, 16, 2);
    fdn.set_feedback(0.0f);
    fdn.set_cross_feedback(0.0f);
    fdn.set_dry(0.0f);
    fdn.set_wet(1.0f);
    fdn.set_crossfade_samples(0);
    fdn.set_linear_smoothing_samples(4);
    fdn.set_delay_samples(2, 2);

    AudioBuffer first_block(2, 8, k_sample_rate);
    for (size_t i = 0; i < first_block.get_num_frames(); ++i) {
        first_block.get_write_pointer(0)[i] = static_cast<float>(i + 1);
    }
    fdn.process(AudioBufferView(first_block));

    fdn.set_delay_sample(StereoFDN::k_left, 0, 4);

    AudioBuffer second_block(2, 6, k_sample_rate);
    for (size_t i = 0; i < second_block.get_num_frames(); ++i) {
        second_block.get_write_pointer(0)[i] = static_cast<float>(i + 9);
    }
    fdn.process(AudioBufferView(second_block));

    const float* left = second_block.get_read_pointer(0);

    EXPECT_FLOAT_EQ(left[0], 6.5f);
    EXPECT_FLOAT_EQ(left[1], 7.0f);
    EXPECT_FLOAT_EQ(left[2], 7.5f);
    EXPECT_FLOAT_EQ(left[3], 8.0f);
    EXPECT_FLOAT_EQ(left[4], 9.0f);
}

TEST(StereoFDN, SmoothedDelayChangesDoNotRestartCrossfade) {
    StereoFDN fdn;
    fdn.prepare(k_sample_rate, 16, 2);
    fdn.set_feedback(0.0f);
    fdn.set_cross_feedback(0.0f);
    fdn.set_dry(0.0f);
    fdn.set_wet(1.0f);
    fdn.set_crossfade_samples(4);
    fdn.set_linear_smoothing_samples(4);
    fdn.set_delay_samples(2, 2);

    AudioBuffer first_block(2, 8, k_sample_rate);
    for (size_t i = 0; i < first_block.get_num_frames(); ++i) {
        first_block.get_write_pointer(0)[i] = static_cast<float>(i + 1);
    }
    fdn.process(AudioBufferView(first_block));

    fdn.set_delay_sample(StereoFDN::k_left, 0, 4);

    AudioBuffer second_block(2, 6, k_sample_rate);
    for (size_t i = 0; i < second_block.get_num_frames(); ++i) {
        second_block.get_write_pointer(0)[i] = static_cast<float>(i + 9);
    }
    fdn.process(AudioBufferView(second_block));

    const float* left = second_block.get_read_pointer(0);

    EXPECT_FLOAT_EQ(left[0], 6.5f);
    EXPECT_FLOAT_EQ(left[1], 7.0f);
    EXPECT_FLOAT_EQ(left[2], 7.5f);
    EXPECT_FLOAT_EQ(left[3], 8.0f);
    EXPECT_FLOAT_EQ(left[4], 9.0f);
}

TEST(StereoFDN, LargeDelayRetargetsAreSlewLimited) {
    StereoFDN fdn;
    fdn.prepare(k_sample_rate, 16, 2);
    fdn.set_feedback(0.0f);
    fdn.set_cross_feedback(0.0f);
    fdn.set_dry(0.0f);
    fdn.set_wet(1.0f);
    fdn.set_crossfade_samples(0);
    fdn.set_linear_smoothing_samples(4);
    fdn.set_delay_samples(2, 2);

    AudioBuffer first_block(2, 8, k_sample_rate);
    for (size_t i = 0; i < first_block.get_num_frames(); ++i) {
        first_block.get_write_pointer(0)[i] = static_cast<float>(i + 1);
    }
    fdn.process(AudioBufferView(first_block));

    fdn.set_delay_sample(StereoFDN::k_left, 0, 10);

    AudioBuffer second_block(2, 6, k_sample_rate);
    for (size_t i = 0; i < second_block.get_num_frames(); ++i) {
        second_block.get_write_pointer(0)[i] = static_cast<float>(i + 9);
    }
    fdn.process(AudioBufferView(second_block));

    EXPECT_FLOAT_EQ(second_block.get_read_pointer(0)[0], 6.5f);
}

TEST(StereoFDN, SmoothsFeedbackChanges) {
    StereoFDN fdn;
    fdn.prepare(k_sample_rate, 16, 2);
    fdn.set_feedback(0.0f);
    fdn.set_cross_feedback(0.0f);
    fdn.set_dry(0.0f);
    fdn.set_wet(1.0f);
    fdn.set_crossfade_samples(0);
    fdn.set_linear_smoothing_samples(4);
    fdn.set_delay_samples(2, 2);

    AudioBuffer first_block(2, 4, k_sample_rate);
    fdn.process(AudioBufferView(first_block));

    fdn.set_feedback(0.8f);

    AudioBuffer second_block(2, 6, k_sample_rate);
    second_block.get_write_pointer(0)[0] = 1.0f;
    fdn.process(AudioBufferView(second_block));

    const float* left = second_block.get_read_pointer(0);

    EXPECT_FLOAT_EQ(left[0], 0.0f);
    EXPECT_FLOAT_EQ(left[2], 1.0f);
    EXPECT_FLOAT_EQ(left[4], 0.6f);
}

TEST(StereoFDN, CrossFeedbackMovesEnergyBetweenChannels) {
    StereoFDN fdn;
    prepare_for_unit_delay_test(fdn);
    fdn.set_delay_samples(3, 3);
    fdn.set_cross_feedback(1.0f);

    AudioBuffer buffer(2, 8, k_sample_rate);
    buffer.get_write_pointer(0)[0] = 1.0f;

    fdn.process(AudioBufferView(buffer));

    const float* left = buffer.get_read_pointer(0);
    const float* right = buffer.get_read_pointer(1);

    EXPECT_FLOAT_EQ(left[3], 1.0f);
    EXPECT_FLOAT_EQ(left[6], 0.0f);
    EXPECT_FLOAT_EQ(right[6], -0.5f);
}

TEST(StereoFDN, InputPanScalesDryPathAndDelayInput) {
    StereoFDN fdn;
    fdn.prepare(k_sample_rate, 16, 2);
    fdn.set_feedback(0.0f);
    fdn.set_cross_feedback(0.0f);
    fdn.set_input_pan(-1.0f);
    fdn.set_dry(1.0f);
    fdn.set_wet(1.0f);
    fdn.set_linear_smoothing_samples(0);
    fdn.set_delay_samples(2, 2);

    AudioBuffer buffer(2, 6, k_sample_rate);
    buffer.get_write_pointer(0)[0] = 1.0f;
    buffer.get_write_pointer(1)[0] = 1.0f;

    fdn.process(AudioBufferView(buffer));

    const float* left = buffer.get_read_pointer(0);
    const float* right = buffer.get_read_pointer(1);

    EXPECT_FLOAT_EQ(left[0], 1.0f);
    EXPECT_FLOAT_EQ(right[0], 0.0f);
    EXPECT_FLOAT_EQ(left[2], 1.0f);
    EXPECT_FLOAT_EQ(right[2], 0.0f);
}

TEST(StereoFDN, HardInputPanAndFullCrossPingPongsWithoutDualInputOverlap) {
    StereoFDN fdn;
    prepare_for_unit_delay_test(fdn);
    fdn.set_delay_samples(2, 2);
    fdn.set_cross_feedback(1.0f);
    fdn.set_input_pan(-1.0f);

    AudioBuffer buffer(2, 8, k_sample_rate);
    buffer.get_write_pointer(0)[0] = 1.0f;
    buffer.get_write_pointer(1)[0] = 1.0f;

    fdn.process(AudioBufferView(buffer));

    const float* left = buffer.get_read_pointer(0);
    const float* right = buffer.get_read_pointer(1);

    EXPECT_FLOAT_EQ(left[2], 1.0f);
    EXPECT_FLOAT_EQ(right[2], 0.0f);
    EXPECT_FLOAT_EQ(left[4], 0.0f);
    EXPECT_FLOAT_EQ(right[4], -0.5f);
    EXPECT_FLOAT_EQ(left[6], -0.25f);
    EXPECT_FLOAT_EQ(right[6], 0.0f);
}
