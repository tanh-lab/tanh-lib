#include <gtest/gtest.h>
#include "tanh/dsp/audio/AudioBuffer.h"

#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

using namespace thl::dsp::audio;

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
    for (size_t i = 0; i < 64; ++i) {
        block[i] = static_cast<float>(i);
    }

    block.clear();

    for (size_t i = 0; i < 64; ++i) {
        EXPECT_FLOAT_EQ(block[i], 0.0f);
    }
}

TEST(MemoryBlock, CopyConstruction) {
    MemoryBlock<float> original(32);
    for (size_t i = 0; i < 32; ++i) {
        original[i] = static_cast<float>(i) * 0.1f;
    }

    MemoryBlock<float> copy(original);
    EXPECT_EQ(copy.size(), 32u);
    EXPECT_NE(copy.data(), original.data());

    for (size_t i = 0; i < 32; ++i) {
        EXPECT_FLOAT_EQ(copy[i], original[i]);
    }
}

TEST(MemoryBlock, CopyAssignment) {
    MemoryBlock<float> original(32);
    for (size_t i = 0; i < 32; ++i) {
        original[i] = static_cast<float>(i);
    }

    MemoryBlock<float> copy(16);
    copy = original;
    EXPECT_EQ(copy.size(), 32u);

    for (size_t i = 0; i < 32; ++i) {
        EXPECT_FLOAT_EQ(copy[i], original[i]);
    }
}

TEST(MemoryBlock, MoveConstruction) {
    MemoryBlock<float> original(64);
    float* originalPtr = original.data();

    MemoryBlock<float> moved(std::move(original));
    EXPECT_EQ(moved.size(), 64u);
    EXPECT_EQ(moved.data(), originalPtr);
    EXPECT_EQ(original.size(), 0u);
    EXPECT_EQ(original.data(), nullptr);
}

TEST(MemoryBlock, MoveAssignment) {
    MemoryBlock<float> original(64);
    float* originalPtr = original.data();

    MemoryBlock<float> moved;
    moved = std::move(original);
    EXPECT_EQ(moved.size(), 64u);
    EXPECT_EQ(moved.data(), originalPtr);
    EXPECT_EQ(original.size(), 0u);
    EXPECT_EQ(original.data(), nullptr);
}

TEST(MemoryBlock, Resize) {
    MemoryBlock<float> block(32);
    for (size_t i = 0; i < 32; ++i) {
        block[i] = static_cast<float>(i);
    }

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

TEST(MemoryBlock, SwapDataRawPointer) {
    MemoryBlock<float> block(4);
    for (size_t i = 0; i < 4; ++i) {
        block[i] = 1.0f;
    }

    auto* raw = static_cast<float*>(std::malloc(4 * sizeof(float)));
    for (size_t i = 0; i < 4; ++i) {
        raw[i] = 3.0f;
    }

    block.swap_data(raw, 4);

    for (size_t i = 0; i < 4; ++i) {
        EXPECT_FLOAT_EQ(block[i], 3.0f);
        EXPECT_FLOAT_EQ(raw[i], 1.0f);
    }

    std::free(raw);
}

TEST(MemoryBlock, SubscriptOperator) {
    MemoryBlock<float> block(8);
    for (size_t i = 0; i < 8; ++i) {
        block[i] = static_cast<float>(i * 10);
    }

    for (size_t i = 0; i < 8; ++i) {
        EXPECT_FLOAT_EQ(block[i], static_cast<float>(i * 10));
    }

    const MemoryBlock<float>& cref = block;
    EXPECT_FLOAT_EQ(cref[3], 30.0f);
}

// =============================================================================
// Buffer<T> / AudioBuffer Tests
// =============================================================================

TEST(AudioBuffer, DefaultConstruction) {
    AudioBuffer buffer;
    EXPECT_EQ(buffer.get_num_channels(), 0u);
    EXPECT_EQ(buffer.get_num_frames(), 0u);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 0.0);
    EXPECT_TRUE(buffer.empty());
}

TEST(AudioBuffer, SizedConstruction) {
    AudioBuffer buffer(2, 128, 44100.0);
    EXPECT_EQ(buffer.get_num_channels(), 2u);
    EXPECT_EQ(buffer.get_num_frames(), 128u);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 44100.0);
    EXPECT_FALSE(buffer.empty());
}

TEST(AudioBuffer, ConstructionInitialisesZero) {
    AudioBuffer buffer(2, 64, 48000.0);
    for (size_t ch = 0; ch < 2; ++ch) {
        const float* ptr = buffer.get_read_pointer(ch);
        for (size_t f = 0; f < 64; ++f) {
            EXPECT_FLOAT_EQ(ptr[f], 0.0f);
        }
    }
}

TEST(AudioBuffer, WriteAndReadPointers) {
    AudioBuffer buffer(2, 64, 48000.0);

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

TEST(AudioBuffer, ReadWritePointerWithOffset) {
    AudioBuffer buffer(1, 128, 48000.0);
    float* ptr = buffer.get_write_pointer(0);
    for (size_t f = 0; f < 128; ++f) {
        ptr[f] = static_cast<float>(f);
    }

    const float* offset = buffer.get_read_pointer(0, 64);
    EXPECT_FLOAT_EQ(*offset, 64.0f);

    float* wOffset = buffer.get_write_pointer(0, 64);
    EXPECT_FLOAT_EQ(*wOffset, 64.0f);
}

TEST(AudioBuffer, GetSetSample) {
    AudioBuffer buffer(2, 32, 48000.0);
    buffer.set_sample(0, 10, 0.5f);
    buffer.set_sample(1, 20, -0.75f);

    EXPECT_FLOAT_EQ(buffer.get_sample(0, 10), 0.5f);
    EXPECT_FLOAT_EQ(buffer.get_sample(1, 20), -0.75f);
    EXPECT_FLOAT_EQ(buffer.get_sample(0, 0), 0.0f);
}

TEST(AudioBuffer, Clear) {
    AudioBuffer buffer(2, 64, 48000.0);
    float* ch0 = buffer.get_write_pointer(0);
    for (size_t f = 0; f < 64; ++f) {
        ch0[f] = 1.0f;
    }

    buffer.clear();

    for (size_t f = 0; f < 64; ++f) {
        EXPECT_FLOAT_EQ(buffer.get_sample(0, f), 0.0f);
    }
}

TEST(AudioBuffer, SetSize) {
    AudioBuffer buffer;
    buffer.set_size(3, 256, 96000.0);

    EXPECT_EQ(buffer.get_num_channels(), 3u);
    EXPECT_EQ(buffer.get_num_frames(), 256u);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 96000.0);
    EXPECT_FALSE(buffer.empty());
}

TEST(AudioBuffer, Resize) {
    AudioBuffer buffer(2, 64, 48000.0);
    buffer.resize(4, 128);

    EXPECT_EQ(buffer.get_num_channels(), 4u);
    EXPECT_EQ(buffer.get_num_frames(), 128u);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 48000.0);
}

TEST(AudioBuffer, CopyConstruction) {
    AudioBuffer original(2, 64, 44100.0);
    for (size_t ch = 0; ch < 2; ++ch) {
        float* ptr = original.get_write_pointer(ch);
        for (size_t f = 0; f < 64; ++f) {
            ptr[f] = static_cast<float>(ch * 64 + f);
        }
    }

    AudioBuffer copy(original);
    EXPECT_EQ(copy.get_num_channels(), 2u);
    EXPECT_EQ(copy.get_num_frames(), 64u);
    EXPECT_DOUBLE_EQ(copy.get_sample_rate(), 44100.0);

    EXPECT_NE(copy.data(), original.data());

    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t f = 0; f < 64; ++f) {
            EXPECT_FLOAT_EQ(copy.get_sample(ch, f),
                            original.get_sample(ch, f));
        }
    }
}

TEST(AudioBuffer, CopyAssignment) {
    AudioBuffer original(2, 32, 44100.0);
    original.set_sample(0, 0, 42.0f);

    AudioBuffer copy;
    copy = original;
    EXPECT_EQ(copy.get_num_channels(), 2u);
    EXPECT_EQ(copy.get_num_frames(), 32u);
    EXPECT_FLOAT_EQ(copy.get_sample(0, 0), 42.0f);
}

TEST(AudioBuffer, MoveConstruction) {
    AudioBuffer original(2, 64, 48000.0);
    original.set_sample(0, 0, 7.0f);
    float* originalData = original.data();

    AudioBuffer moved(std::move(original));
    EXPECT_EQ(moved.get_num_channels(), 2u);
    EXPECT_EQ(moved.get_num_frames(), 64u);
    EXPECT_DOUBLE_EQ(moved.get_sample_rate(), 48000.0);
    EXPECT_EQ(moved.data(), originalData);
    EXPECT_FLOAT_EQ(moved.get_sample(0, 0), 7.0f);

    EXPECT_EQ(original.get_num_channels(), 0u);
    EXPECT_EQ(original.get_num_frames(), 0u);
}

TEST(AudioBuffer, MoveAssignment) {
    AudioBuffer original(2, 64, 48000.0);
    original.set_sample(1, 10, 3.14f);

    AudioBuffer moved;
    moved = std::move(original);
    EXPECT_EQ(moved.get_num_channels(), 2u);
    EXPECT_FLOAT_EQ(moved.get_sample(1, 10), 3.14f);
    EXPECT_EQ(original.get_num_channels(), 0u);
}

TEST(AudioBuffer, SelfCopyAssignment) {
    AudioBuffer buffer(2, 32, 44100.0);
    buffer.set_sample(0, 0, 1.0f);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"
    buffer = buffer;
#pragma clang diagnostic pop

    EXPECT_EQ(buffer.get_num_channels(), 2u);
    EXPECT_FLOAT_EQ(buffer.get_sample(0, 0), 1.0f);
}

TEST(AudioBuffer, SelfMoveAssignment) {
    AudioBuffer buffer(2, 32, 44100.0);
    buffer.set_sample(0, 0, 1.0f);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
    buffer = std::move(buffer);
#pragma clang diagnostic pop

    EXPECT_EQ(buffer.get_num_channels(), 2u);
    EXPECT_FLOAT_EQ(buffer.get_sample(0, 0), 1.0f);
}

TEST(AudioBuffer, ArrayOfPointers) {
    AudioBuffer buffer(3, 64, 48000.0);
    for (size_t ch = 0; ch < 3; ++ch) {
        buffer.set_sample(ch, 0, static_cast<float>(ch + 1));
    }

    const float* const* readPtrs = buffer.get_array_of_read_pointers();
    EXPECT_FLOAT_EQ(readPtrs[0][0], 1.0f);
    EXPECT_FLOAT_EQ(readPtrs[1][0], 2.0f);
    EXPECT_FLOAT_EQ(readPtrs[2][0], 3.0f);

    float* const* writePtrs = buffer.get_array_of_write_pointers();
    writePtrs[0][0] = 10.0f;
    EXPECT_FLOAT_EQ(buffer.get_sample(0, 0), 10.0f);
}

TEST(AudioBuffer, DataPointer) {
    AudioBuffer buffer(2, 32, 48000.0);
    EXPECT_NE(buffer.data(), nullptr);
    EXPECT_EQ(buffer.data(), buffer.get_memory_block().data());
}

TEST(AudioBuffer, SwapDataBuffers) {
    AudioBuffer a(2, 32, 48000.0);
    AudioBuffer b(2, 32, 48000.0);

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

TEST(AudioBuffer, SwapDataMemoryBlock) {
    AudioBuffer buffer(1, 4, 48000.0);
    for (size_t f = 0; f < 4; ++f) {
        buffer.set_sample(0, f, 1.0f);
    }

    MemoryBlock<float> block(4);
    for (size_t i = 0; i < 4; ++i) {
        block[i] = 5.0f;
    }

    buffer.swap_data(block);

    for (size_t f = 0; f < 4; ++f) {
        EXPECT_FLOAT_EQ(buffer.get_sample(0, f), 5.0f);
        EXPECT_FLOAT_EQ(block[f], 1.0f);
    }
}

TEST(AudioBuffer, SwapDataRawPointer) {
    AudioBuffer buffer(1, 4, 48000.0);
    for (size_t f = 0; f < 4; ++f) {
        buffer.set_sample(0, f, 1.0f);
    }

    auto* raw = static_cast<float*>(std::malloc(4 * sizeof(float)));
    for (size_t i = 0; i < 4; ++i) {
        raw[i] = 9.0f;
    }

    buffer.swap_data(raw, 4);

    for (size_t f = 0; f < 4; ++f) {
        EXPECT_FLOAT_EQ(buffer.get_sample(0, f), 9.0f);
        EXPECT_FLOAT_EQ(raw[f], 1.0f);
    }

    std::free(raw);
}

TEST(AudioBuffer, ResetChannelPointers) {
    AudioBuffer buffer(2, 32, 48000.0);
    float* ch0Before = buffer.get_write_pointer(0);

    buffer.reset_channel_ptr();

    EXPECT_EQ(buffer.get_write_pointer(0), ch0Before);
    EXPECT_EQ(buffer.get_write_pointer(1), buffer.data() + 32);
}

TEST(AudioBuffer, PlanarLayout) {
    AudioBuffer buffer(3, 16, 48000.0);
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

TEST(AudioBuffer, ToInterleaved) {
    AudioBuffer buffer(2, 4, 48000.0);
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

TEST(AudioBuffer, FromInterleaved) {
    float data[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    AudioBuffer buffer = from_interleaved(data, 2, 4, 44100.0);

    EXPECT_EQ(buffer.get_num_channels(), 2u);
    EXPECT_EQ(buffer.get_num_frames(), 4u);
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

TEST(AudioBuffer, InterleavedRoundTrip) {
    AudioBuffer original(2, 64, 48000.0);
    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t f = 0; f < 64; ++f) {
            original.set_sample(
                ch, f,
                std::sin(static_cast<float>(ch * 64 + f) * 0.1f));
        }
    }

    std::vector<float> interleaved = to_interleaved(original);
    AudioBuffer reconstructed =
        from_interleaved(interleaved.data(), 2, 64, 48000.0);

    for (size_t ch = 0; ch < 2; ++ch) {
        for (size_t f = 0; f < 64; ++f) {
            EXPECT_FLOAT_EQ(reconstructed.get_sample(ch, f),
                            original.get_sample(ch, f))
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
    EXPECT_EQ(buffer.get_num_frames(), 32u);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 96000.0);

    buffer.set_sample(0, 0, 3.14159);
    EXPECT_DOUBLE_EQ(buffer.get_sample(0, 0), 3.14159);

    buffer.clear();
    EXPECT_DOUBLE_EQ(buffer.get_sample(0, 0), 0.0);
}
