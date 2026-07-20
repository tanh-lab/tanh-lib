#include <gtest/gtest.h>

#include <array>

#include "tanh/core/RingBuffer.h"

using namespace thl::core;

TEST(RingBuffer, DefaultConstruction) {
    RingBuffer<float> rb;
    EXPECT_EQ(rb.get_num_channels(), 0u);
    EXPECT_EQ(rb.get_num_samples(), 0u);
}

TEST(RingBuffer, InitialiseWithPositions) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(2, 128);
    EXPECT_EQ(rb.get_num_channels(), 2u);
    EXPECT_EQ(rb.get_num_samples(), 128u);
    EXPECT_EQ(rb.get_available_samples(0), 0u);
    EXPECT_EQ(rb.get_available_samples(1), 0u);
}

TEST(RingBuffer, PushPopSingle) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 8);
    rb.push_sample(0, 1.0f);
    rb.push_sample(0, 2.0f);
    rb.push_sample(0, 3.0f);
    EXPECT_EQ(rb.get_available_samples(0), 3u);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 1.0f);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 2.0f);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 3.0f);
    EXPECT_EQ(rb.get_available_samples(0), 0u);
}

TEST(RingBuffer, WrapAround) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 4);
    for (int i = 0; i < 4; ++i) { rb.push_sample(0, static_cast<float>(i)); }
    EXPECT_EQ(rb.get_available_samples(0), 4u);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 0.0f);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 1.0f);
    rb.push_sample(0, 10.0f);
    rb.push_sample(0, 11.0f);
    EXPECT_EQ(rb.get_available_samples(0), 4u);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 2.0f);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 3.0f);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 10.0f);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 11.0f);
}

TEST(RingBuffer, PushPopBlock) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 16);
    std::array<float, 4> in = {1.0f, 2.0f, 3.0f, 4.0f};
    rb.push_block(0, in.data(), 4);
    EXPECT_EQ(rb.get_available_samples(0), 4u);
    std::array<float, 4> out = {};
    rb.pop_block(0, out.data(), 4);
    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[3], 4.0f);
    EXPECT_EQ(rb.get_available_samples(0), 0u);
}

TEST(RingBuffer, PushBlockWrapAround) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 4);
    std::array<float, 3> in1 = {1, 2, 3};
    rb.push_block(0, in1.data(), 3);
    rb.pop_sample(0);
    rb.pop_sample(0);
    std::array<float, 3> in2 = {10, 11, 12};
    rb.push_block(0, in2.data(), 3);
    EXPECT_EQ(rb.get_available_samples(0), 4u);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 3.0f);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 10.0f);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 11.0f);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 12.0f);
}

TEST(RingBuffer, PopBlockWrapAround) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 4);
    for (int i = 0; i < 4; ++i) { rb.push_sample(0, static_cast<float>(i)); }
    rb.pop_sample(0);
    rb.pop_sample(0);
    rb.push_sample(0, 10.0f);
    rb.push_sample(0, 11.0f);
    std::array<float, 4> out = {};
    rb.pop_block(0, out.data(), 4);
    EXPECT_FLOAT_EQ(out[0], 2.0f);
    EXPECT_FLOAT_EQ(out[1], 3.0f);
    EXPECT_FLOAT_EQ(out[2], 10.0f);
    EXPECT_FLOAT_EQ(out[3], 11.0f);
}

TEST(RingBuffer, FutureSample) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 8);
    rb.push_sample(0, 10.0f);
    rb.push_sample(0, 20.0f);
    rb.push_sample(0, 30.0f);
    EXPECT_FLOAT_EQ(rb.get_future_sample(0, 0), 10.0f);
    EXPECT_FLOAT_EQ(rb.get_future_sample(0, 1), 20.0f);
    EXPECT_FLOAT_EQ(rb.get_future_sample(0, 2), 30.0f);
    EXPECT_EQ(rb.get_available_samples(0), 3u);
}

TEST(RingBuffer, PastSample) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 8);
    rb.push_sample(0, 1.0f);
    rb.push_sample(0, 2.0f);
    rb.push_sample(0, 3.0f);
    rb.pop_sample(0);
    rb.pop_sample(0);
    EXPECT_EQ(rb.get_available_past_samples(0), 2u);
    EXPECT_FLOAT_EQ(rb.get_past_sample(0, 1), 2.0f);
    EXPECT_FLOAT_EQ(rb.get_past_sample(0, 2), 1.0f);
}

TEST(RingBuffer, MultiChannelIndependence) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(2, 8);
    rb.push_sample(0, 100.0f);
    rb.push_sample(1, 200.0f);
    rb.push_sample(1, 300.0f);
    EXPECT_EQ(rb.get_available_samples(0), 1u);
    EXPECT_EQ(rb.get_available_samples(1), 2u);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 100.0f);
    EXPECT_FLOAT_EQ(rb.pop_sample(1), 200.0f);
}

TEST(RingBuffer, ClearWithPositions) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 8);
    rb.push_sample(0, 1.0f);
    rb.push_sample(0, 2.0f);
    rb.clear_with_positions();
    EXPECT_EQ(rb.get_available_samples(0), 0u);
    EXPECT_EQ(rb.get_available_past_samples(0), 0u);
}

TEST(RingBuffer, FullState) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 4);
    rb.push_sample(0, 1.0f);
    rb.push_sample(0, 2.0f);
    rb.push_sample(0, 3.0f);
    rb.push_sample(0, 4.0f);
    EXPECT_EQ(rb.get_available_samples(0), 4u);
}

// --- typed element support -------------------------------------------------

TEST(RingBufferTyped, Int64RoundTripIsExact) {
    RingBuffer<int64_t> rb;
    rb.initialize_with_positions(1, 8);  // American-spelling alias
    // Values beyond float32's 2^24 integer range must survive exactly.
    const int64_t values[] = {0, -1, 13087, (1LL << 40) + 7, INT64_MAX};
    for (const int64_t v : values) { rb.push_sample(0, v); }
    EXPECT_EQ(rb.get_available_samples(0), 5u);
    for (const int64_t v : values) { EXPECT_EQ(rb.pop_sample(0), v); }
    EXPECT_EQ(rb.get_available_samples(0), 0u);
}

TEST(RingBufferTyped, PopEmptyReturnsValueInitialized) {
    RingBuffer<int32_t> rb;
    rb.initialise_with_positions(1, 4);
    EXPECT_EQ(rb.pop_sample(0), 0);
    rb.push_sample(0, 42);
    EXPECT_EQ(rb.pop_sample(0), 42);
    EXPECT_EQ(rb.pop_sample(0), 0);
}

TEST(RingBufferTyped, BlockOpsWithInt16) {
    RingBuffer<int16_t> rb;
    rb.initialise_with_positions(2, 16);
    const int16_t in[] = {1, -2, 3, -4};
    rb.push_block(1, in, 4);
    EXPECT_EQ(rb.get_available_samples(1), 4u);
    int16_t out[4] = {};
    rb.pop_block(1, out, 4);
    for (int i = 0; i < 4; ++i) { EXPECT_EQ(out[i], in[i]); }
}

TEST(RingBufferTyped, OverwriteOldestWhenFull) {
    RingBuffer<int32_t> rb;
    rb.initialise_with_positions(1, 4);
    for (int32_t v = 1; v <= 6; ++v) { rb.push_sample(0, v); }  // 5 and 6 overwrite 1 and 2
    EXPECT_EQ(rb.get_available_samples(0), 4u);
    EXPECT_EQ(rb.pop_sample(0), 3);
    EXPECT_EQ(rb.pop_sample(0), 4);
    EXPECT_EQ(rb.pop_sample(0), 5);
    EXPECT_EQ(rb.pop_sample(0), 6);
}
