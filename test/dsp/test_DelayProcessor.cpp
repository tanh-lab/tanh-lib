#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/utils/DelayProcessor.h>

#include <cstddef>

namespace {

constexpr double k_sample_rate = 48000.0;

using thl::core::BufferF;
using thl::core::BufferView;
using thl::dsp::utils::DelayProcessor;

// One channel of `num_samples` with 1.0 at `position` and 0.0 elsewhere.
BufferF impulse(size_t num_samples, size_t position) {
    BufferF buffer(1, num_samples, k_sample_rate);
    buffer.get_write_pointer(0)[position] = 1.0f;
    return buffer;
}

// One channel holding frame + 1 at each frame, starting at `first_frame`.
BufferF ramp(size_t first_frame, size_t num_samples) {
    BufferF buffer(1, num_samples, k_sample_rate);
    for (size_t i = 0; i < num_samples; ++i) {
        buffer.get_write_pointer(0)[i] = static_cast<float>(first_frame + i + 1);
    }
    return buffer;
}

// The ramp's value at `frame` when delayed by `delay`, or 0 before the ramp started.
float delayed(size_t frame, size_t delay) {
    return frame >= delay ? static_cast<float>(frame - delay + 1) : 0.0f;
}

void expect_impulse_at(const BufferF& buffer, size_t channel, size_t position) {
    const float* data = buffer.get_read_pointer(channel);
    for (size_t i = 0; i < buffer.get_num_samples(); ++i) {
        EXPECT_FLOAT_EQ(data[i], i == position ? 1.0f : 0.0f) << "sample " << i;
    }
}

}  // namespace

TEST(DelayProcessor, DelaysAnImpulseByTheSetDelay) {
    DelayProcessor delay(8);
    delay.prepare(k_sample_rate, 12, 1);
    delay.set_delay_samples(3);

    BufferF buffer = impulse(12, 0);
    delay.process(BufferView(buffer));

    expect_impulse_at(buffer, 0, 3);
}

TEST(DelayProcessor, DelayStateContinuesAcrossBlocks) {
    DelayProcessor delay(8);
    delay.prepare(k_sample_rate, 4, 1);
    delay.set_delay_samples(5);

    BufferF first = impulse(4, 0);
    delay.process(BufferView(first));
    BufferF second(1, 4, k_sample_rate);
    delay.process(BufferView(second));

    expect_impulse_at(first, 0, 4);  // nothing in the first block
    expect_impulse_at(second, 0, 1);
}

TEST(DelayProcessor, MaxDelayThatIsAPowerOfTwoFits) {
    DelayProcessor delay(16);
    delay.prepare(k_sample_rate, 20, 1);
    delay.set_delay_samples(16);

    BufferF buffer = impulse(20, 0);
    delay.process(BufferView(buffer));

    expect_impulse_at(buffer, 0, 16);
}

TEST(DelayProcessor, ZeroDelayPassesThroughAndKeepsHistory) {
    DelayProcessor delay(8);
    delay.prepare(k_sample_rate, 8, 1);
    delay.set_crossfade_samples(0);

    BufferF buffer(1, 8, k_sample_rate);
    for (size_t i = 0; i < 8; ++i) { buffer.get_write_pointer(0)[i] = static_cast<float>(i + 1); }
    delay.process(BufferView(buffer));
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(buffer.get_read_pointer(0)[i], static_cast<float>(i + 1));
    }

    delay.set_delay_samples(4);
    BufferF next(1, 4, k_sample_rate);
    delay.process(BufferView(next));
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(next.get_read_pointer(0)[i], static_cast<float>(i + 5));
    }
}

TEST(DelayProcessor, WriteKeepsHistoryWithoutChangingTheBuffer) {
    DelayProcessor delay(8);
    delay.prepare(k_sample_rate, 8, 1);
    delay.set_delay_samples(4);

    BufferF buffer(1, 8, k_sample_rate);
    for (size_t i = 0; i < 8; ++i) { buffer.get_write_pointer(0)[i] = static_cast<float>(i + 1); }
    delay.write(BufferView(buffer));
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(buffer.get_read_pointer(0)[i], static_cast<float>(i + 1));
    }

    BufferF next(1, 4, k_sample_rate);
    delay.process(BufferView(next));
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(next.get_read_pointer(0)[i], static_cast<float>(i + 5));
    }
}

TEST(DelayProcessor, NoMaxDelayPassesThrough) {
    DelayProcessor delay;
    delay.prepare(k_sample_rate, 8, 1);
    delay.set_delay_samples(4);

    BufferF buffer = impulse(8, 2);
    delay.process(BufferView(buffer));

    EXPECT_EQ(delay.delay_samples(), 0u);
    expect_impulse_at(buffer, 0, 2);
}

TEST(DelayProcessor, DelayIsClampedToTheMax) {
    DelayProcessor delay(4);
    delay.prepare(k_sample_rate, 8, 1);
    delay.set_delay_samples(10);

    BufferF buffer = impulse(8, 0);
    delay.process(BufferView(buffer));

    EXPECT_EQ(delay.delay_samples(), 4u);
    expect_impulse_at(buffer, 0, 4);
}

TEST(DelayProcessor, ChannelsAreDelayedIndependently) {
    DelayProcessor delay(8);
    delay.prepare(k_sample_rate, 8, 2);
    delay.set_delay_samples(3);

    BufferF buffer(2, 8, k_sample_rate);
    buffer.get_write_pointer(0)[0] = 1.0f;
    buffer.get_write_pointer(1)[2] = 1.0f;
    delay.process(BufferView(buffer));

    expect_impulse_at(buffer, 0, 3);
    expect_impulse_at(buffer, 1, 5);
}

TEST(DelayProcessor, ResetClearsTheHistory) {
    DelayProcessor delay(8);
    delay.prepare(k_sample_rate, 4, 1);
    delay.set_delay_samples(6);

    BufferF first = impulse(4, 0);
    delay.process(BufferView(first));
    delay.reset();
    BufferF second(1, 4, k_sample_rate);
    delay.process(BufferView(second));

    expect_impulse_at(second, 0, 4);  // the impulse is gone
}

TEST(DelayProcessor, SetMaxDelayAfterPrepareReallocates) {
    DelayProcessor delay(4);
    delay.prepare(k_sample_rate, 12, 1);
    delay.set_max_delay_samples(10);
    delay.set_delay_samples(10);

    BufferF buffer = impulse(12, 0);
    delay.process(BufferView(buffer));

    EXPECT_EQ(delay.delay_samples(), 10u);
    expect_impulse_at(buffer, 0, 10);
}

TEST(DelayProcessor, CrossfadesLinearlyToANewDelay) {
    DelayProcessor delay(16);
    delay.prepare(k_sample_rate, 8, 1);
    delay.set_crossfade_samples(4);
    delay.set_delay_samples(2);

    BufferF first = ramp(0, 8);
    delay.process(BufferView(first));
    delay.set_delay_samples(6);
    BufferF second = ramp(8, 8);
    delay.process(BufferView(second));

    for (size_t i = 0; i < 8; ++i) {
        const size_t frame = 8 + i;
        const float previous = delayed(frame, 2);
        const float current = delayed(frame, 6);
        const float expected =
            i < 4 ? previous + (current - previous) * static_cast<float>(i) / 4.0f : current;
        EXPECT_FLOAT_EQ(second.get_read_pointer(0)[i], expected) << "frame " << frame;
    }
}

TEST(DelayProcessor, CrossfadeOfZeroSwitchesImmediately) {
    DelayProcessor delay(16);
    delay.prepare(k_sample_rate, 8, 1);
    delay.set_crossfade_samples(0);
    delay.set_delay_samples(2);

    BufferF first = ramp(0, 8);
    delay.process(BufferView(first));
    delay.set_delay_samples(6);
    BufferF second = ramp(8, 8);
    delay.process(BufferView(second));

    for (size_t i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(second.get_read_pointer(0)[i], delayed(8 + i, 6)) << "frame " << 8 + i;
    }
}

TEST(DelayProcessor, DelaySetDuringACrossfadeRestartsFromTheDelayBeingFadedTo) {
    DelayProcessor delay(16);
    delay.prepare(k_sample_rate, 8, 1);
    delay.set_crossfade_samples(4);
    delay.set_delay_samples(2);

    BufferF first = ramp(0, 8);
    delay.process(BufferView(first));
    delay.set_delay_samples(6);
    BufferF second = ramp(8, 2);  // halfway through the crossfade from 2 to 6
    delay.process(BufferView(second));
    delay.set_delay_samples(10);
    BufferF third = ramp(10, 8);
    delay.process(BufferView(third));

    for (size_t i = 0; i < 8; ++i) {
        const size_t frame = 10 + i;
        const float previous = delayed(frame, 6);
        const float current = delayed(frame, 10);
        const float expected =
            i < 4 ? previous + (current - previous) * static_cast<float>(i) / 4.0f : current;
        EXPECT_FLOAT_EQ(third.get_read_pointer(0)[i], expected) << "frame " << frame;
    }
}

TEST(DelayProcessor, WriteAdvancesTheCrossfade) {
    DelayProcessor delay(16);
    delay.prepare(k_sample_rate, 8, 1);
    delay.set_crossfade_samples(4);
    delay.set_delay_samples(2);

    BufferF first = ramp(0, 8);
    delay.process(BufferView(first));
    delay.set_delay_samples(6);
    BufferF written = ramp(8, 4);  // the whole crossfade passes while only writing
    delay.write(BufferView(written));
    BufferF next = ramp(12, 4);
    delay.process(BufferView(next));

    for (size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(next.get_read_pointer(0)[i], delayed(12 + i, 6)) << "frame " << 12 + i;
    }
}

TEST(DelayProcessor, ReportsNoLatency) {
    DelayProcessor delay(8);
    delay.prepare(k_sample_rate, 8, 1);
    delay.set_delay_samples(8);

    EXPECT_EQ(delay.latency_samples(), 0u);
    EXPECT_EQ(delay.max_latency_samples(), 0u);
}
