#include <gtest/gtest.h>

#include <array>

#include "tanh/dsp/audio/RingBuffer.h"

using namespace thl::dsp::audio;

TEST(RingBuffer, DefaultConstruction) {
    RingBuffer rb;
    EXPECT_EQ(rb.get_num_channels(), 0u);
    EXPECT_EQ(rb.get_num_samples(), 0u);
}

TEST(RingBuffer, InitialiseWithPositions) {
    RingBuffer rb;
    rb.initialise_with_positions(2, 128);
    EXPECT_EQ(rb.get_num_channels(), 2u);
    EXPECT_EQ(rb.get_num_samples(), 128u);
    EXPECT_EQ(rb.get_available_samples(0), 0u);
    EXPECT_EQ(rb.get_available_samples(1), 0u);
}

TEST(RingBuffer, PushPopSingle) {
    RingBuffer rb;
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
    RingBuffer rb;
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
    RingBuffer rb;
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
    RingBuffer rb;
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
    RingBuffer rb;
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
    RingBuffer rb;
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
    RingBuffer rb;
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
    RingBuffer rb;
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
    RingBuffer rb;
    rb.initialise_with_positions(1, 8);
    rb.push_sample(0, 1.0f);
    rb.push_sample(0, 2.0f);
    rb.clear_with_positions();
    EXPECT_EQ(rb.get_available_samples(0), 0u);
    EXPECT_EQ(rb.get_available_past_samples(0), 0u);
}

TEST(RingBuffer, FullState) {
    RingBuffer rb;
    rb.initialise_with_positions(1, 4);
    rb.push_sample(0, 1.0f);
    rb.push_sample(0, 2.0f);
    rb.push_sample(0, 3.0f);
    rb.push_sample(0, 4.0f);
    EXPECT_EQ(rb.get_available_samples(0), 4u);
}
