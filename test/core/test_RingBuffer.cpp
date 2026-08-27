#include <gtest/gtest.h>

#include <array>
#include <random>
#include <vector>

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

TEST(RingBufferTyped, ZeroCapacityIsGraceful) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 0);
    rb.push_sample(0, 1.0f);  // must not divide by zero
    EXPECT_EQ(rb.get_available_samples(0), 0u);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 0.0f);
    EXPECT_FLOAT_EQ(rb.get_future_sample(0, 0), 0.0f);
    EXPECT_FLOAT_EQ(rb.get_past_sample(0, 0), 0.0f);
}

// ---------------------------------------------------------------------------
// Bulk push/pop must be indistinguishable from repeated single-sample calls.
// ---------------------------------------------------------------------------

namespace {

// Minimal per-sample reference implementation of the ring semantics.
struct ReferenceRing {
    std::vector<int> buf;
    size_t read = 0, write = 0, valid = 0;
    bool full = false;

    explicit ReferenceRing(size_t capacity) : buf(capacity, 0) {}

    void push(int v) {
        const size_t cap = buf.size();
        buf[write] = v;
        write = (write + 1) % cap;
        if (full) {
            read = write;
        } else if (valid < cap) {
            ++valid;
        }
        full = (write == read);
    }

    bool empty() const { return !full && read == write; }

    int pop() {
        if (empty()) { return 0; }
        const int v = buf[read];
        read = (read + 1) % buf.size();
        full = false;
        return v;
    }

    size_t available() const {
        const size_t cap = buf.size();
        return full ? cap : (write + cap - read) % cap;
    }
};

}  // namespace

TEST(RingBufferBulk, DifferentialAgainstPerSample) {
    std::mt19937 rng(0xC0FFEE);
    for (const size_t capacity : {1U, 2U, 3U, 7U, 8U, 64U}) {
        thl::core::RingBuffer<int> rb;
        rb.initialise_with_positions(1, capacity);
        ReferenceRing ref(capacity);
        std::uniform_int_distribution<size_t> len(0, capacity * 2 + 3);
        std::uniform_int_distribution<int> coin(0, 1);
        int next = 1;

        for (int step = 0; step < 2000; ++step) {
            const size_t n = len(rng);
            if (coin(rng) == 0) {
                std::vector<int> in(n);
                for (auto& v : in) { v = next++; }
                rb.push_block(0, in.data(), n);
                for (const int v : in) { ref.push(v); }
            } else {
                std::vector<int> out(n, -1);
                rb.pop_block(0, out.data(), n);
                for (size_t i = 0; i < n; ++i) {
                    ASSERT_EQ(out[i], ref.pop()) << "cap=" << capacity << " step=" << step;
                }
            }
            ASSERT_EQ(rb.get_available_samples(0), ref.available())
                << "cap=" << capacity << " step=" << step;
            ASSERT_EQ(rb.get_available_past_samples(0), ref.valid - ref.available())
                << "cap=" << capacity << " step=" << step;
            for (size_t off = 0; off < capacity; ++off) {
                ASSERT_EQ(rb.get_future_sample(0, off), ref.buf[(ref.read + off) % capacity]);
            }
        }
    }
}

TEST(RingBufferBulk, PushLargerThanCapacityKeepsTail) {
    thl::core::RingBuffer<int> rb;
    rb.initialise_with_positions(1, 4);
    const int in[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    rb.push_block(0, in, 10);
    EXPECT_EQ(rb.get_available_samples(0), 4U);
    int out[4] = {};
    rb.pop_block(0, out, 4);
    EXPECT_EQ(out[0], 7);
    EXPECT_EQ(out[1], 8);
    EXPECT_EQ(out[2], 9);
    EXPECT_EQ(out[3], 10);
}

TEST(RingBufferBulk, PushOverflowingPartiallyFullDragsReadPos) {
    thl::core::RingBuffer<int> rb;
    rb.initialise_with_positions(1, 4);
    const int a[] = {1, 2, 3};
    rb.push_block(0, a, 3);
    const int b[] = {4, 5, 6};  // overflows by 2: 1 and 2 are dropped
    rb.push_block(0, b, 3);
    EXPECT_EQ(rb.get_available_samples(0), 4U);
    int out[4] = {};
    rb.pop_block(0, out, 4);
    EXPECT_EQ(out[0], 3);
    EXPECT_EQ(out[1], 4);
    EXPECT_EQ(out[2], 5);
    EXPECT_EQ(out[3], 6);
}

TEST(RingBufferBulk, PushExactlyFillingMarksFull) {
    thl::core::RingBuffer<int> rb;
    rb.initialise_with_positions(1, 3);
    const int in[] = {1, 2, 3};
    rb.push_block(0, in, 3);
    EXPECT_EQ(rb.get_available_samples(0), 3U);
    EXPECT_EQ(rb.get_future_sample(0, 0), 1);
}

TEST(RingBufferBulk, PopMoreThanAvailableZeroFillsTail) {
    thl::core::RingBuffer<int> rb;
    rb.initialise_with_positions(1, 8);
    const int in[] = {5, 6};
    rb.push_block(0, in, 2);
    int out[5] = {9, 9, 9, 9, 9};
    rb.pop_block(0, out, 5);
    EXPECT_EQ(out[0], 5);
    EXPECT_EQ(out[1], 6);
    for (int i = 2; i < 5; ++i) { EXPECT_EQ(out[i], 0); }
    EXPECT_EQ(rb.get_available_samples(0), 0U);
}

TEST(RingBufferBulk, ZeroCountIsNoOp) {
    thl::core::RingBuffer<int> rb;
    rb.initialise_with_positions(1, 4);
    const int in[] = {1};
    rb.push_block(0, in, 1);
    rb.push_block(0, nullptr, 0);
    rb.pop_block(0, nullptr, 0);
    EXPECT_EQ(rb.get_available_samples(0), 1U);
}
