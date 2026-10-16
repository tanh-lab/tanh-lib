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

TEST(DelayProcessor, ReportsNoLatency) {
    DelayProcessor delay(8);
    delay.prepare(k_sample_rate, 8, 1);
    delay.set_delay_samples(8);

    EXPECT_EQ(delay.latency_samples(), 0u);
    EXPECT_EQ(delay.max_latency_samples(), 0u);
}
