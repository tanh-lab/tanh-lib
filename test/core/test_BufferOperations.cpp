#include <gtest/gtest.h>

#include <array>
#include <cstddef>

#include "tanh/core/Buffer.h"
#include "tanh/core/BufferOperations.h"
#include "tanh/core/BufferView.h"

using namespace thl::core;

namespace {

// Sample n of channel c holds 100 * c + n.
Buffer<float> ramp(size_t num_channels, size_t num_samples) {
    Buffer<float> buffer(num_channels, num_samples);
    for (size_t channel = 0; channel < num_channels; ++channel) {
        for (size_t n = 0; n < num_samples; ++n) {
            buffer.set_sample(channel, n, static_cast<float>(100 * channel + n));
        }
    }
    return buffer;
}

Buffer<float> filled(size_t num_channels, size_t num_samples, float value) {
    Buffer<float> buffer(num_channels, num_samples);
    for (size_t channel = 0; channel < num_channels; ++channel) {
        for (size_t n = 0; n < num_samples; ++n) { buffer.set_sample(channel, n, value); }
    }
    return buffer;
}

}  // namespace

TEST(BufferOperations, CopyCopiesEveryChannel) {
    const Buffer<float> source = ramp(2, 8);
    Buffer<float> destination = filled(2, 8, 0.0f);

    BufferOperations::copy(destination, source);

    for (size_t channel = 0; channel < 2; ++channel) {
        for (size_t n = 0; n < 8; ++n) {
            EXPECT_FLOAT_EQ(destination.get_sample(channel, n), source.get_sample(channel, n));
        }
    }
}

TEST(BufferOperations, AddAddsEveryChannel) {
    const Buffer<float> source = ramp(2, 8);
    Buffer<float> destination = filled(2, 8, 1.0f);

    BufferOperations::add(destination, source);

    for (size_t channel = 0; channel < 2; ++channel) {
        for (size_t n = 0; n < 8; ++n) {
            EXPECT_FLOAT_EQ(destination.get_sample(channel, n),
                            1.0f + source.get_sample(channel, n));
        }
    }
}

TEST(BufferOperations, WorksOnSubBlocks) {
    Buffer<float> source = ramp(2, 8);
    Buffer<float> destination = filled(2, 8, 0.0f);

    BufferOperations::copy(BufferView(destination).sub_block(0, 4),
                           BufferView(source).sub_block(4, 4));
    BufferOperations::add(BufferView(destination).sub_block(4, 4),
                          BufferView(source).sub_block(0, 4));

    for (size_t channel = 0; channel < 2; ++channel) {
        for (size_t n = 0; n < 4; ++n) {
            EXPECT_FLOAT_EQ(destination.get_sample(channel, n), source.get_sample(channel, n + 4));
            EXPECT_FLOAT_EQ(destination.get_sample(channel, n + 4), source.get_sample(channel, n));
        }
    }
}

TEST(BufferOperations, WorksOnMonoViews) {
    std::array<float, 4> source = {1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> destination = {10.0f, 10.0f, 10.0f, 10.0f};

    BufferOperations::add(BufferView(destination.data(), 4), ConstBufferView(source.data(), 4));

    EXPECT_EQ(destination, (std::array<float, 4>{11.0f, 12.0f, 13.0f, 14.0f}));
}

#if !defined(NDEBUG) && GTEST_HAS_DEATH_TEST
TEST(BufferOperationsDeathTest, AssertsTheDimensions) {
    const Buffer<float> source = ramp(2, 8);
    Buffer<float> fewer_channels = filled(1, 8, 0.0f);
    Buffer<float> fewer_samples = filled(2, 4, 0.0f);

    // RTSan off for the call that asserts: its abort would be reported before the message.
    EXPECT_DEATH(
        {
            TANH_NONBLOCKING_SCOPED_DISABLER
            BufferOperations::copy(fewer_channels, source);
        },
        "different dimensions");
    EXPECT_DEATH(
        {
            TANH_NONBLOCKING_SCOPED_DISABLER
            BufferOperations::add(fewer_samples, source);
        },
        "different dimensions");
}
#endif
