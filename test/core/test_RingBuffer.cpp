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
    EXPECT_EQ(rb.get_available_past_samples(0), 0u);
    const float in[] = {1.0f, 2.0f};
    rb.push_block(0, in, 2);
    float out[2] = {9.0f, 9.0f};
    rb.pop_block(0, out, 2);
    EXPECT_FLOAT_EQ(out[0], 0.0f);
    EXPECT_FLOAT_EQ(out[1], 0.0f);
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

// =============================================================================
// Index wrapping is done with compare-and-subtract; these pin the behaviour
// against the plain modulo definition, including offsets at and beyond the
// capacity where a single subtract would not be enough.
// =============================================================================

TEST(RingBufferBulk, PushFillMatchesRepeatedPush) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 8);
    rb.push_sample(0, 1.0f);
    rb.push_sample(0, 2.0f);
    rb.pop_sample(0);
    rb.push_fill(0, 0.5f, 5);  // wraps around the seam
    EXPECT_EQ(rb.get_available_samples(0), 6u);
    std::array<float, 6> out = {};
    rb.pop_block(0, out.data(), 6);
    EXPECT_FLOAT_EQ(out[0], 2.0f);
    for (size_t i = 1; i < 6; ++i) { EXPECT_FLOAT_EQ(out[i], 0.5f) << i; }

    // Larger than the capacity: only the last `capacity` survive, ring is full.
    rb.push_fill(0, 7.0f, 20);
    EXPECT_EQ(rb.get_available_samples(0), 8u);
    for (size_t i = 0; i < 8; ++i) { EXPECT_FLOAT_EQ(rb.get_future_sample(0, i), 7.0f) << i; }
}

TEST(RingBufferBulk, DiscardMatchesRepeatedPop) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 8);
    for (int i = 0; i < 6; ++i) { rb.push_sample(0, static_cast<float>(i)); }
    EXPECT_EQ(rb.discard(0, 4), 4u);
    EXPECT_EQ(rb.get_available_samples(0), 2u);
    EXPECT_EQ(rb.get_available_past_samples(0), 4u);
    EXPECT_FLOAT_EQ(rb.pop_sample(0), 4.0f);
    EXPECT_EQ(rb.discard(0, 10), 1u);  // clamps to what is available
    EXPECT_EQ(rb.get_available_samples(0), 0u);
    EXPECT_EQ(rb.discard(0, 1), 0u);
}

TEST(RingBufferBulk, PeekPastBlockMatchesGetPastSample) {
    RingBuffer<float> rb;
    rb.initialise_with_positions(1, 8);
    for (int i = 0; i < 8; ++i) { rb.push_sample(0, static_cast<float>(i)); }
    rb.discard(0, 5);  // history = 0 1 2 3 4, read position on 5

    std::array<float, 3> past = {};
    rb.peek_past_block(0, past.data(), 3);
    EXPECT_FLOAT_EQ(past[0], 2.0f);
    EXPECT_FLOAT_EQ(past[1], 3.0f);
    EXPECT_FLOAT_EQ(past[2], 4.0f);
    for (size_t k = 0; k < 3; ++k) { EXPECT_FLOAT_EQ(past[k], rb.get_past_sample(0, 3 - k)); }

    // Reading further back than the capacity wraps like get_past_sample() does.
    std::array<float, 11> wrapped = {};
    rb.peek_past_block(0, wrapped.data(), 11);
    for (size_t k = 0; k < 11; ++k) {
        EXPECT_FLOAT_EQ(wrapped[k], rb.get_past_sample(0, 11 - k)) << k;
    }
    EXPECT_EQ(rb.get_available_samples(0), 3u);  // peek does not consume
}

// push_fill / discard / peek_past_block against the per-sample API itself, with
// the fresh-plus-history window read an anira-style pre-processor performs.
TEST(RingBufferBulk, FillDiscardPeekDifferentialAgainstPerSample) {
    std::mt19937 rng(0xF00D);
    for (const size_t capacity : {1U, 2U, 3U, 7U, 8U, 64U}) {
        RingBuffer<float> per_sample;
        RingBuffer<float> bulk;
        per_sample.initialise_with_positions(1, capacity);
        bulk.initialise_with_positions(1, capacity);
        std::uniform_int_distribution<size_t> len(0, capacity * 2 + 3);
        std::uniform_int_distribution<int> op(0, 3);
        float next = 1.0f;

        for (int step = 0; step < 2000; ++step) {
            const size_t n = len(rng);
            switch (op(rng)) {
                case 0: {
                    for (size_t i = 0; i < n; ++i) { per_sample.push_sample(0, next); }
                    bulk.push_fill(0, next, n);
                    next += 1.0f;
                    break;
                }
                case 1: {
                    std::vector<float> in(n);
                    for (auto& v : in) { v = next++; }
                    for (const float v : in) { per_sample.push_sample(0, v); }
                    bulk.push_block(0, in.data(), n);
                    break;
                }
                case 2: {
                    for (size_t i = 0; i < n; ++i) { per_sample.pop_sample(0); }
                    bulk.discard(0, n);
                    break;
                }
                default: {
                    // Window = [num_old history][num_new fresh]; never asks for more
                    // fresh samples than are available (the caller's contract).
                    const size_t num_new = n % (per_sample.get_available_samples(0) + 1);
                    const size_t num_old = len(rng);
                    std::vector<float> expected(num_old + num_new);
                    std::vector<float> actual(num_old + num_new);
                    for (size_t k = 0; k < num_old; ++k) {
                        expected[k] = per_sample.get_past_sample(0, num_old - k);
                    }
                    for (size_t k = 0; k < num_new; ++k) {
                        expected[num_old + k] = per_sample.pop_sample(0);
                    }
                    bulk.peek_past_block(0, actual.data(), num_old);
                    bulk.pop_block(0, actual.data() + num_old, num_new);
                    ASSERT_EQ(actual, expected) << "cap=" << capacity << " step=" << step;
                    break;
                }
            }
            ASSERT_EQ(bulk.get_available_samples(0), per_sample.get_available_samples(0))
                << "cap=" << capacity << " step=" << step;
            ASSERT_EQ(bulk.get_available_past_samples(0), per_sample.get_available_past_samples(0))
                << "cap=" << capacity << " step=" << step;
            for (size_t off = 0; off < capacity; ++off) {
                ASSERT_EQ(bulk.get_future_sample(0, off), per_sample.get_future_sample(0, off));
                ASSERT_EQ(bulk.get_past_sample(0, off), per_sample.get_past_sample(0, off));
            }
        }
    }
}

TEST(RingBufferWrap, FutureAndPastOffsetsMatchModuloReference) {
    constexpr size_t k_capacity = 7;
    RingBuffer<int> rb;
    rb.initialise_with_positions(1, k_capacity);
    // Leave the read position mid-buffer so every offset crosses the end.
    for (int i = 0; i < 5; ++i) { rb.push_sample(0, i); }
    for (int i = 0; i < 3; ++i) { (void)rb.pop_sample(0); }
    for (int i = 5; i < 5 + static_cast<int>(k_capacity); ++i) { rb.push_sample(0, i); }
    // Ring now holds the last k_capacity pushes (5..11) starting at read_pos.

    std::array<int, k_capacity> expected{};
    for (size_t i = 0; i < k_capacity; ++i) { expected[i] = rb.get_future_sample(0, i); }

    for (size_t offset = 0; offset < 4 * k_capacity; ++offset) {
        EXPECT_EQ(rb.get_future_sample(0, offset), expected[offset % k_capacity])
            << "future offset " << offset;
        const size_t past_index = (k_capacity - (offset % k_capacity)) % k_capacity;
        EXPECT_EQ(rb.get_past_sample(0, offset), expected[past_index]) << "past offset " << offset;
    }
}

TEST(RingBufferWrap, AvailableSamplesAcrossTheSeam) {
    constexpr size_t k_capacity = 8;
    RingBuffer<int> rb;
    rb.initialise_with_positions(1, k_capacity);
    // Move both positions past the seam without ever filling the ring.
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 6; ++i) { rb.push_sample(0, i); }
        EXPECT_EQ(rb.get_available_samples(0), 6u);
        for (int i = 0; i < 6; ++i) { EXPECT_EQ(rb.pop_sample(0), i); }
        EXPECT_EQ(rb.get_available_samples(0), 0u);
    }
}
