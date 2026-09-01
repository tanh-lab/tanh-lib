#include <gtest/gtest.h>
#include <tanh/core/Numbers.h>

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "tanh/core/Buffer.h"

using namespace thl::core;

// =============================================================================
// MemoryBlock Tests
// =============================================================================

TEST(MemoryBlock, DefaultConstruction) {
    MemoryBlock<float> block;
    EXPECT_EQ(block.size(), 0u);
    EXPECT_EQ(block.data(), nullptr);
}

TEST(MemoryBlock, SizedConstruction) {
    MemoryBlock<float> block(128);
    EXPECT_EQ(block.size(), 128u);
    EXPECT_NE(block.data(), nullptr);
}

TEST(MemoryBlock, ZeroSizedConstruction) {
    MemoryBlock<float> block(0);
    EXPECT_EQ(block.size(), 0u);
}

TEST(MemoryBlock, ClearZeroesData) {
    MemoryBlock<float> block(64);
    for (size_t i = 0; i < 64; ++i) { block[i] = static_cast<float>(i); }

    block.clear();

    for (size_t i = 0; i < 64; ++i) { EXPECT_FLOAT_EQ(block[i], 0.0f); }
}

TEST(MemoryBlock, CopyConstruction) {
    MemoryBlock<float> original(32);
    for (size_t i = 0; i < 32; ++i) { original[i] = static_cast<float>(i) * 0.1f; }

    MemoryBlock<float> copy(original);
    EXPECT_EQ(copy.size(), 32u);
    EXPECT_NE(copy.data(), original.data());

    for (size_t i = 0; i < 32; ++i) { EXPECT_FLOAT_EQ(copy[i], original[i]); }
}

TEST(MemoryBlock, CopyAssignment) {
    MemoryBlock<float> original(32);
    for (size_t i = 0; i < 32; ++i) { original[i] = static_cast<float>(i); }

    MemoryBlock<float> copy(16);
    copy = original;
    EXPECT_EQ(copy.size(), 32u);

    for (size_t i = 0; i < 32; ++i) { EXPECT_FLOAT_EQ(copy[i], original[i]); }
}

TEST(MemoryBlock, MoveConstruction) {
    MemoryBlock<float> original(64);
    float* original_ptr = original.data();
    size_t original_size = original.size();

    MemoryBlock<float> moved(std::move(original));
    EXPECT_EQ(moved.size(), original_size);
    EXPECT_EQ(moved.data(), original_ptr);
}

TEST(MemoryBlock, MoveAssignment) {
    MemoryBlock<float> original(64);
    float* original_ptr = original.data();
    size_t original_size = original.size();

    MemoryBlock<float> moved;
    moved = std::move(original);
    EXPECT_EQ(moved.size(), original_size);
    EXPECT_EQ(moved.data(), original_ptr);
}

TEST(MemoryBlock, Resize) {
    MemoryBlock<float> block(32);
    for (size_t i = 0; i < 32; ++i) { block[i] = static_cast<float>(i); }

    block.resize(64);
    EXPECT_EQ(block.size(), 64u);

    for (size_t i = 0; i < 32; ++i) {
        EXPECT_FLOAT_EQ(block[i], static_cast<float>(i))
            << "Preserved data mismatch at index " << i;
    }
}

TEST(MemoryBlock, ResizeFromEmpty) {
    MemoryBlock<float> block;
    block.resize(128);
    EXPECT_EQ(block.size(), 128u);
    EXPECT_NE(block.data(), nullptr);
}

TEST(MemoryBlock, SwapData) {
    MemoryBlock<float> a(4);
    MemoryBlock<float> b(4);

    for (size_t i = 0; i < 4; ++i) {
        a[i] = 1.0f;
        b[i] = 2.0f;
    }

    a.swap_data(b);

    for (size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(a[i], 2.0f);
        EXPECT_FLOAT_EQ(b[i], 1.0f);
    }
}

TEST(MemoryBlock, SwapDataIsZeroCopy) {
    MemoryBlock<float> a(8);
    MemoryBlock<float> b(8);
    float* const a_data = a.data();
    float* const b_data = b.data();

    a.swap_data(b);

    // The allocations themselves change hands; nothing is copied.
    EXPECT_EQ(a.data(), b_data);
    EXPECT_EQ(b.data(), a_data);
}

TEST(MemoryBlock, SwapDataRawPointer) {
    MemoryBlock<float> block(4);
    for (size_t i = 0; i < 4; ++i) { block[i] = 1.0f; }

    auto* raw = static_cast<float*>(std::malloc(4 * sizeof(float)));
    for (size_t i = 0; i < 4; ++i) { raw[i] = 3.0f; }

    block.swap_data(raw, 4);

    for (size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(block[i], 3.0f);
        EXPECT_FLOAT_EQ(raw[i], 1.0f);
    }

    std::free(raw);
}

TEST(MemoryBlock, SubscriptOperator) {
    MemoryBlock<float> block(8);
    for (size_t i = 0; i < 8; ++i) { block[i] = static_cast<float>(i * 10); }

    for (size_t i = 0; i < 8; ++i) { EXPECT_FLOAT_EQ(block[i], static_cast<float>(i * 10)); }

    const MemoryBlock<float>& cref = block;
    EXPECT_FLOAT_EQ(cref[3], 30.0f);
}

// =============================================================================
// Buffer<T> / BufferF Tests
// =============================================================================

TEST(BufferF, DefaultConstruction) {
    BufferF buffer;
    EXPECT_EQ(buffer.get_num_channels(), 0u);
    EXPECT_EQ(buffer.get_num_samples(), 0u);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 0.0);
    EXPECT_TRUE(buffer.empty());
}

TEST(BufferF, SizedConstruction) {
    BufferF buffer(2, 128, 44100.0);
    EXPECT_EQ(buffer.get_num_channels(), 2u);
    EXPECT_EQ(buffer.get_num_samples(), 128u);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 44100.0);
    EXPECT_FALSE(buffer.empty());
}

TEST(BufferF, ConstructionInitialisesZero) {
    BufferF buffer(2, 64, 48000.0);
    for (size_t ch = 0; ch < 2; ++ch) {
        const float* ptr = buffer.get_read_pointer(ch);
        for (size_t f = 0; f < 64; ++f) { EXPECT_FLOAT_EQ(ptr[f], 0.0f); }
    }
}

TEST(BufferF, WriteAndReadPointers) {
    BufferF buffer(2, 64, 48000.0);

    float* ch0 = buffer.get_write_pointer(0);
    float* ch1 = buffer.get_write_pointer(1);

    for (size_t f = 0; f < 64; ++f) {
        ch0[f] = static_cast<float>(f);
        ch1[f] = static_cast<float>(f) * -1.0f;
    }

    const float* r0 = buffer.get_read_pointer(0);
    const float* r1 = buffer.get_read_pointer(1);

    for (size_t f = 0; f < 64; ++f) {
        EXPECT_FLOAT_EQ(r0[f], static_cast<float>(f));
        EXPECT_FLOAT_EQ(r1[f], static_cast<float>(f) * -1.0f);
    }
}

TEST(BufferF, ReadWritePointerWithOffset) {
    BufferF buffer(1, 128, 48000.0);
    float* ptr = buffer.get_write_pointer(0);
    for (size_t f = 0; f < 128; ++f) { ptr[f] = static_cast<float>(f); }

    const float* offset = buffer.get_read_pointer(0, 64);
    EXPECT_FLOAT_EQ(*offset, 64.0f);

    float* w_offset = buffer.get_write_pointer(0, 64);
    EXPECT_FLOAT_EQ(*w_offset, 64.0f);
}

TEST(BufferF, GetSetSample) {
    BufferF buffer(2, 32, 48000.0);
    buffer.set_sample(0, 10, 0.5f);
    buffer.set_sample(1, 20, -0.75f);

    EXPECT_FLOAT_EQ(buffer.get_sample(0, 10), 0.5f);
    EXPECT_FLOAT_EQ(buffer.get_sample(1, 20), -0.75f);
    EXPECT_FLOAT_EQ(buffer.get_sample(0, 0), 0.0f);
}

TEST(BufferF, Clear) {
    BufferF buffer(2, 64, 48000.0);
    float* ch0 = buffer.get_write_pointer(0);
    for (size_t f = 0; f < 64; ++f) { ch0[f] = 1.0f; }

    buffer.clear();

    for (size_t f = 0; f < 64; ++f) { EXPECT_FLOAT_EQ(buffer.get_sample(0, f), 0.0f); }
}

TEST(BufferF, SetSize) {
    BufferF buffer;
    buffer.set_size(3, 256, 96000.0);

    EXPECT_EQ(buffer.get_num_channels(), 3u);
    EXPECT_EQ(buffer.get_num_samples(), 256u);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 96000.0);
    EXPECT_FALSE(buffer.empty());
}

TEST(BufferF, Resize) {
    BufferF buffer(2, 64, 48000.0);
    buffer.resize(4, 128);

    EXPECT_EQ(buffer.get_num_channels(), 4u);
    EXPECT_EQ(buffer.get_num_samples(), 128u);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 48000.0);
}

TEST(BufferF, CopyConstruction) {
    BufferF original(2, 64, 44100.0);
    for (size_t ch = 0; ch < 2; ++ch) {
        float* ptr = original.get_write_pointer(ch);
        for (size_t f = 0; f < 64; ++f) { ptr[f] = static_cast<float>(ch * 64 + f); }
    }

    BufferF copy(original);
    EXPECT_EQ(copy.get_num_channels(), 2u);
    EXPECT_EQ(copy.get_num_samples(), 64u);
    EXPECT_DOUBLE_EQ(copy.get_sample_rate(), 44100.0);

    EXPECT_NE(copy.data(), original.data());

    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t f = 0; f < 64; ++f) {
            EXPECT_FLOAT_EQ(copy.get_sample(ch, f), original.get_sample(ch, f));
        }
    }
}

TEST(BufferF, CopyAssignment) {
    BufferF original(2, 32, 44100.0);
    original.set_sample(0, 0, 42.0f);

    BufferF copy;
    copy = original;
    EXPECT_EQ(copy.get_num_channels(), 2u);
    EXPECT_EQ(copy.get_num_samples(), 32u);
    EXPECT_FLOAT_EQ(copy.get_sample(0, 0), 42.0f);
}

TEST(BufferF, MoveConstruction) {
    BufferF original(2, 64, 48000.0);
    original.set_sample(0, 0, 7.0f);
    float* original_data = original.data();

    BufferF moved(std::move(original));
    EXPECT_EQ(moved.get_num_channels(), 2u);
    EXPECT_EQ(moved.get_num_samples(), 64u);
    EXPECT_DOUBLE_EQ(moved.get_sample_rate(), 48000.0);
    EXPECT_EQ(moved.data(), original_data);
    EXPECT_FLOAT_EQ(moved.get_sample(0, 0), 7.0f);
}

TEST(BufferF, MoveAssignment) {
    BufferF original(2, 64, 48000.0);
    original.set_sample(1, 10, 3.14f);

    BufferF moved;
    moved = std::move(original);
    EXPECT_EQ(moved.get_num_channels(), 2u);
    EXPECT_FLOAT_EQ(moved.get_sample(1, 10), 3.14f);
}

TEST(BufferF, SelfCopyAssignment) {
    BufferF buffer(2, 32, 44100.0);
    buffer.set_sample(0, 0, 1.0f);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"
    buffer = buffer;
#pragma clang diagnostic pop

    EXPECT_EQ(buffer.get_num_channels(), 2u);
    EXPECT_FLOAT_EQ(buffer.get_sample(0, 0), 1.0f);
}

TEST(BufferF, SelfMoveAssignment) {
    BufferF buffer(2, 32, 44100.0);
    buffer.set_sample(0, 0, 1.0f);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
    buffer = std::move(buffer);
#pragma clang diagnostic pop

    EXPECT_EQ(buffer.get_num_channels(), 2u);
    EXPECT_FLOAT_EQ(buffer.get_sample(0, 0), 1.0f);
}

TEST(BufferF, ArrayOfPointers) {
    BufferF buffer(3, 64, 48000.0);
    for (size_t ch = 0; ch < 3; ++ch) { buffer.set_sample(ch, 0, static_cast<float>(ch + 1)); }

    const float* const* read_ptrs = buffer.get_array_of_read_pointers();
    EXPECT_FLOAT_EQ(read_ptrs[0][0], 1.0f);
    EXPECT_FLOAT_EQ(read_ptrs[1][0], 2.0f);
    EXPECT_FLOAT_EQ(read_ptrs[2][0], 3.0f);

    float* const* write_ptrs = buffer.get_array_of_write_pointers();
    write_ptrs[0][0] = 10.0f;
    EXPECT_FLOAT_EQ(buffer.get_sample(0, 0), 10.0f);
}

TEST(BufferF, DataPointer) {
    BufferF buffer(2, 32, 48000.0);
    EXPECT_NE(buffer.data(), nullptr);
    EXPECT_EQ(buffer.data(), buffer.get_memory_block().data());
}

TEST(BufferF, SwapDataBuffers) {
    BufferF a(2, 32, 48000.0);
    BufferF b(2, 32, 48000.0);

    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t f = 0; f < 32; ++f) {
            a.set_sample(ch, f, 1.0f);
            b.set_sample(ch, f, 2.0f);
        }
    }

    a.swap_data(b);

    EXPECT_FLOAT_EQ(a.get_sample(0, 0), 2.0f);
    EXPECT_FLOAT_EQ(b.get_sample(0, 0), 1.0f);
}

TEST(BufferF, SwapDataBuffersIsZeroCopy) {
    BufferF a(2, 32, 48000.0);
    BufferF b(2, 32, 48000.0);

    float* const a_data = a.data();
    float* const b_data = b.data();
    const float* const a_ch0 = a.get_read_pointer(0);
    const float* const a_ch1 = a.get_read_pointer(1);
    const float* const b_ch0 = b.get_read_pointer(0);
    const float* const b_ch1 = b.get_read_pointer(1);

    a.swap_data(b);

    // The storage and the channel pointers themselves change hands; nothing
    // is copied.
    EXPECT_EQ(a.data(), b_data);
    EXPECT_EQ(b.data(), a_data);
    EXPECT_EQ(a.get_read_pointer(0), b_ch0);
    EXPECT_EQ(a.get_read_pointer(1), b_ch1);
    EXPECT_EQ(b.get_read_pointer(0), a_ch0);
    EXPECT_EQ(b.get_read_pointer(1), a_ch1);
}

TEST(BufferF, SwapDataMemoryBlock) {
    BufferF buffer(1, 4, 48000.0);
    for (size_t f = 0; f < 4; ++f) { buffer.set_sample(0, f, 1.0f); }

    MemoryBlock<float> block(4);
    for (size_t i = 0; i < 4; ++i) { block[i] = 5.0f; }

    buffer.swap_data(block);

    for (size_t f = 0; f < 4; ++f) {
        EXPECT_FLOAT_EQ(buffer.get_sample(0, f), 5.0f);
        EXPECT_FLOAT_EQ(block[f], 1.0f);
    }
}

TEST(BufferF, SwapDataMemoryBlockIsZeroCopy) {
    BufferF buffer(2, 16, 48000.0);
    MemoryBlock<float> block(32);

    float* const buffer_data = buffer.data();
    float* const block_data = block.data();

    buffer.swap_data(block);

    // The blocks exchange their allocations and the channel pointers are
    // rebuilt onto the adopted storage; nothing is copied.
    EXPECT_EQ(buffer.data(), block_data);
    EXPECT_EQ(block.data(), buffer_data);
    EXPECT_EQ(buffer.get_read_pointer(0), block_data);
    EXPECT_EQ(buffer.get_read_pointer(1), block_data + 16);
}

TEST(BufferF, SwapDataRawPointer) {
    BufferF buffer(1, 4, 48000.0);
    for (size_t f = 0; f < 4; ++f) { buffer.set_sample(0, f, 1.0f); }

    auto* raw = static_cast<float*>(std::malloc(4 * sizeof(float)));
    for (size_t i = 0; i < 4; ++i) { raw[i] = 9.0f; }

    buffer.swap_data(raw, 4);

    for (size_t f = 0; f < 4; ++f) {
        EXPECT_FLOAT_EQ(buffer.get_sample(0, f), 9.0f);
        EXPECT_FLOAT_EQ(raw[f], 1.0f);
    }

    std::free(raw);
}

TEST(BufferF, ResetChannelPointers) {
    BufferF buffer(2, 32, 48000.0);
    float* ch0_before = buffer.get_write_pointer(0);

    buffer.reset_channel_ptr();

    EXPECT_EQ(buffer.get_write_pointer(0), ch0_before);
    EXPECT_EQ(buffer.get_write_pointer(1), buffer.data() + 32);
}

TEST(BufferF, PlanarLayout) {
    BufferF buffer(3, 16, 48000.0);
    float* raw = buffer.data();

    buffer.set_sample(0, 5, 10.0f);
    buffer.set_sample(1, 5, 20.0f);
    buffer.set_sample(2, 5, 30.0f);

    EXPECT_FLOAT_EQ(raw[5], 10.0f);
    EXPECT_FLOAT_EQ(raw[16 + 5], 20.0f);
    EXPECT_FLOAT_EQ(raw[32 + 5], 30.0f);
}

// =============================================================================
// Interleave / De-interleave Free Functions
// =============================================================================

TEST(BufferF, ToInterleaved) {
    BufferF buffer(2, 4, 48000.0);
    for (size_t f = 0; f < 4; ++f) {
        buffer.set_sample(0, f, static_cast<float>(f));
        buffer.set_sample(1, f, static_cast<float>(f) + 10.0f);
    }

    std::vector<float> interleaved = to_interleaved(buffer);
    ASSERT_EQ(interleaved.size(), 8u);

    EXPECT_FLOAT_EQ(interleaved[0], 0.0f);
    EXPECT_FLOAT_EQ(interleaved[1], 10.0f);
    EXPECT_FLOAT_EQ(interleaved[2], 1.0f);
    EXPECT_FLOAT_EQ(interleaved[3], 11.0f);
    EXPECT_FLOAT_EQ(interleaved[4], 2.0f);
    EXPECT_FLOAT_EQ(interleaved[5], 12.0f);
    EXPECT_FLOAT_EQ(interleaved[6], 3.0f);
    EXPECT_FLOAT_EQ(interleaved[7], 13.0f);
}

TEST(BufferF, FromInterleaved) {
    std::array<float, 8> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    BufferF buffer = from_interleaved(data.data(), 2, 4, 44100.0);

    EXPECT_EQ(buffer.get_num_channels(), 2u);
    EXPECT_EQ(buffer.get_num_samples(), 4u);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 44100.0);

    EXPECT_FLOAT_EQ(buffer.get_sample(0, 0), 1.0f);
    EXPECT_FLOAT_EQ(buffer.get_sample(1, 0), 2.0f);
    EXPECT_FLOAT_EQ(buffer.get_sample(0, 1), 3.0f);
    EXPECT_FLOAT_EQ(buffer.get_sample(1, 1), 4.0f);
    EXPECT_FLOAT_EQ(buffer.get_sample(0, 2), 5.0f);
    EXPECT_FLOAT_EQ(buffer.get_sample(1, 2), 6.0f);
    EXPECT_FLOAT_EQ(buffer.get_sample(0, 3), 7.0f);
    EXPECT_FLOAT_EQ(buffer.get_sample(1, 3), 8.0f);
}

TEST(BufferF, InterleavedRoundTrip) {
    BufferF original(2, 64, 48000.0);
    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t f = 0; f < 64; ++f) {
            original.set_sample(ch, f, std::sin(static_cast<float>(ch * 64 + f) * 0.1f));
        }
    }

    std::vector<float> interleaved = to_interleaved(original);
    BufferF reconstructed = from_interleaved(interleaved.data(), 2, 64, 48000.0);

    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t f = 0; f < 64; ++f) {
            EXPECT_FLOAT_EQ(reconstructed.get_sample(ch, f), original.get_sample(ch, f))
                << "Mismatch at ch=" << ch << " frame=" << f;
        }
    }
}

// =============================================================================
// Buffer<double> Template Instantiation
// =============================================================================

TEST(BufferDouble, BasicOperations) {
    Buffer<double> buffer(2, 32, 96000.0);
    EXPECT_EQ(buffer.get_num_channels(), 2u);
    EXPECT_EQ(buffer.get_num_samples(), 32u);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 96000.0);

    buffer.set_sample(0, 0, std::numbers::pi);
    EXPECT_DOUBLE_EQ(buffer.get_sample(0, 0), std::numbers::pi);

    buffer.clear();
    EXPECT_DOUBLE_EQ(buffer.get_sample(0, 0), 0.0);
}

// =============================================================================
// Buffer<int> Template Instantiation
// =============================================================================

TEST(BufferInt, SwapDataBuffers) {
    Buffer<int> a(2, 8);
    Buffer<int> b(2, 8);
    for (size_t f = 0; f < 8; ++f) {
        a.set_sample(0, f, 1);
        a.set_sample(1, f, 2);
        b.set_sample(0, f, 3);
        b.set_sample(1, f, 4);
    }
    int* const a_data = a.data();
    int* const b_data = b.data();

    a.swap_data(b);

    EXPECT_EQ(a.data(), b_data);
    EXPECT_EQ(b.data(), a_data);
    EXPECT_EQ(a.get_sample(0, 0), 3);
    EXPECT_EQ(a.get_sample(1, 7), 4);
    EXPECT_EQ(b.get_sample(0, 0), 1);
    EXPECT_EQ(b.get_sample(1, 7), 2);
}

TEST(BufferInt, SwapDataMemoryBlock) {
    Buffer<int> buffer(1, 4);
    for (size_t f = 0; f < 4; ++f) { buffer.set_sample(0, f, 1); }

    MemoryBlock<int> block(4);
    for (size_t i = 0; i < 4; ++i) { block[i] = 5; }
    int* const block_data = block.data();

    buffer.swap_data(block);

    EXPECT_EQ(buffer.data(), block_data);
    for (size_t f = 0; f < 4; ++f) {
        EXPECT_EQ(buffer.get_sample(0, f), 5);
        EXPECT_EQ(block[f], 1);
    }
}

TEST(Buffer, CopyOfZeroFrameBufferKeepsChannelPointers) {
    // Review regression: the copy ctor / copy assignment used to skip the
    // channel-pointer array when there were no frames, leaving m_channels
    // null while num_channels == 2.
    Buffer<float> a;
    a.resize(2, 0);
    ASSERT_NE(a.get_array_of_write_pointers(), nullptr);

    const Buffer<float> b = a;
    EXPECT_EQ(b.get_num_channels(), 2U);
    EXPECT_EQ(b.get_num_samples(), 0U);
    EXPECT_NE(b.get_array_of_read_pointers(), nullptr);

    Buffer<float> c;
    c = a;
    EXPECT_NE(c.get_array_of_write_pointers(), nullptr);

    // Growing the copy afterwards must work normally.
    c.resize(2, 8);
    c.clear();
    EXPECT_EQ(c.get_num_samples(), 8U);
    EXPECT_NE(c.get_write_pointer(1), nullptr);
}

TEST(MemoryBlock, ResizeToZeroReportsZero) {
    MemoryBlock<float> m(16);
    EXPECT_EQ(m.size(), 16U);
    m.resize(0);
    EXPECT_EQ(m.size(), 0U);
    EXPECT_EQ(m.data(), nullptr);
    m.resize(4);
    EXPECT_EQ(m.size(), 4U);
    EXPECT_NE(m.data(), nullptr);
}

// =============================================================================
// Failure semantics: no logging, bad_alloc on allocation failure, contract
// violations assert in debug and are no-ops in release.
// =============================================================================

// The sanitizer runtimes (ASan/TSan/LSan/MSan) abort on an allocation request
// above their supported maximum instead of returning nullptr, so the
// allocation-failure tests only run in plain builds.
#if !defined(TANH_WITH_ASAN) && !defined(TANH_WITH_TSAN) && !defined(TANH_WITH_LSAN) && \
    !defined(TANH_WITH_MSAN)
namespace {
// Larger than any allocator will hand out; malloc returns nullptr. Only
// meaningful with a 64-bit size_t: on 32-bit targets this is ~2 GiB, which an
// overcommitting kernel may grant, so the tests skip there.
constexpr size_t k_impossible_count = static_cast<size_t>(-1) / (2 * sizeof(float));
constexpr bool k_can_force_alloc_failure = sizeof(size_t) >= 8;
}  // namespace

TEST(MemoryBlockFailure, ConstructionThrowsBadAllocOnFailure) {
    if (!k_can_force_alloc_failure) { GTEST_SKIP() << "needs a 64-bit size_t"; }
    EXPECT_THROW(MemoryBlock<float> block(k_impossible_count), std::bad_alloc);
}

TEST(MemoryBlockFailure, ResizeThrowsBadAllocAndLeavesBlockUnchanged) {
    if (!k_can_force_alloc_failure) { GTEST_SKIP() << "needs a 64-bit size_t"; }
    MemoryBlock<float> block(16);
    block[0] = 1.0F;
    block[15] = 15.0F;

    EXPECT_THROW(block.resize(k_impossible_count), std::bad_alloc);

    // A failed realloc leaves the original allocation valid: size and
    // contents are untouched and the storage is still addressable.
    EXPECT_EQ(block.size(), 16u);
    ASSERT_NE(block.data(), nullptr);
    EXPECT_FLOAT_EQ(block[0], 1.0F);
    EXPECT_FLOAT_EQ(block[15], 15.0F);
}

TEST(BufferFailure, ConstructionThrowsBadAllocOnFailure) {
    if (!k_can_force_alloc_failure) { GTEST_SKIP() << "needs a 64-bit size_t"; }
    EXPECT_THROW(Buffer<float> buffer(1, k_impossible_count), std::bad_alloc);
}

TEST(BufferFailure, ResizeThrowsBadAllocAndLeavesBufferUnchanged) {
    if (!k_can_force_alloc_failure) { GTEST_SKIP() << "needs a 64-bit size_t"; }
    Buffer<float> buffer(2, 16);
    buffer.set_sample(1, 15, 3.0F);

    EXPECT_THROW(buffer.resize(2, k_impossible_count), std::bad_alloc);

    // Dimensions, storage and channel table are all still the old ones.
    EXPECT_EQ(buffer.get_num_channels(), 2u);
    EXPECT_EQ(buffer.get_num_samples(), 16u);
    ASSERT_NE(buffer.get_read_pointer(1), nullptr);
    EXPECT_EQ(buffer.get_read_pointer(1), buffer.data() + 16);
    EXPECT_FLOAT_EQ(buffer.get_sample(1, 15), 3.0F);
}
#endif

#ifdef NDEBUG
TEST(MemoryBlockFailure, SwapDataSizeMismatchIsNoOp) {
    MemoryBlock<float> a(4);
    MemoryBlock<float> b(8);
    float* const a_data = a.data();
    float* const b_data = b.data();

    a.swap_data(b);

    EXPECT_EQ(a.size(), 4u);
    EXPECT_EQ(b.size(), 8u);
    EXPECT_EQ(a.data(), a_data);
    EXPECT_EQ(b.data(), b_data);
}

TEST(BufferFailure, SwapDataDimensionMismatchIsNoOp) {
    Buffer<float> a(2, 4);
    Buffer<float> b(2, 8);
    a.set_sample(1, 3, 42.0F);
    const float* const a_ch1 = a.get_read_pointer(1);

    a.swap_data(b);

    EXPECT_EQ(a.get_num_samples(), 4u);
    EXPECT_EQ(b.get_num_samples(), 8u);
    EXPECT_EQ(a.get_read_pointer(1), a_ch1);
    EXPECT_FLOAT_EQ(a.get_sample(1, 3), 42.0F);
}

TEST(BufferFailure, SwapDataChannelCountMismatchIsNoOp) {
    Buffer<float> a(2, 4);
    Buffer<float> b(4, 4);
    a.set_sample(1, 3, 42.0F);
    const float* const a_ch1 = a.get_read_pointer(1);

    a.swap_data(b);

    EXPECT_EQ(a.get_num_channels(), 2u);
    EXPECT_EQ(b.get_num_channels(), 4u);
    EXPECT_EQ(a.get_read_pointer(1), a_ch1);
    EXPECT_FLOAT_EQ(a.get_sample(1, 3), 42.0F);
}

// Same TOTAL element count on both sides (2x4 vs 4x2, 8 each): a guard that only
// compared total storage size would let this swap through.
TEST(BufferFailure, SwapDataSameTotalDifferentShapeIsNoOp) {
    Buffer<float> a(2, 4);
    Buffer<float> b(4, 2);
    a.set_sample(1, 3, 42.0F);
    const float* const a_ch1 = a.get_read_pointer(1);

    a.swap_data(b);

    EXPECT_EQ(a.get_num_channels(), 2u);
    EXPECT_EQ(a.get_num_samples(), 4u);
    EXPECT_EQ(a.get_read_pointer(1), a_ch1);
    EXPECT_FLOAT_EQ(a.get_sample(1, 3), 42.0F);
}
#elif GTEST_HAS_DEATH_TEST
TEST(MemoryBlockFailureDeathTest, SwapDataSizeMismatchAsserts) {
    MemoryBlock<float> a(4);
    MemoryBlock<float> b(8);
    EXPECT_DEATH(a.swap_data(b), "different sizes");
}

TEST(BufferFailureDeathTest, SwapDataDimensionMismatchAsserts) {
    Buffer<float> a(2, 4);
    Buffer<float> b(2, 8);
    EXPECT_DEATH(a.swap_data(b), "different dimensions");
}

TEST(BufferFailureDeathTest, SwapDataChannelCountMismatchAsserts) {
    Buffer<float> a(2, 4);
    Buffer<float> b(4, 4);
    EXPECT_DEATH(a.swap_data(b), "different dimensions");
}

TEST(BufferFailureDeathTest, SwapDataSameTotalDifferentShapeAsserts) {
    Buffer<float> a(2, 4);
    Buffer<float> b(4, 2);
    EXPECT_DEATH(a.swap_data(b), "different dimensions");
}
#endif
