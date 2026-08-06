#include <gtest/gtest.h>

#include <array>
#include <utility>
#include <vector>

#include "tanh/core/Buffer.h"

using namespace thl::core;

TEST(BufferView, MonoConstructor) {
    std::array<float, 8> data = {};
    BufferView view(data.data(), 8);

    EXPECT_EQ(view.get_num_channels(), 1u);
    EXPECT_EQ(view.get_num_samples(), 8u);
    EXPECT_EQ(view.get_write_pointer(0), data.data());
}

TEST(BufferView, MonoConstructorReadPointer) {
    std::array<float, 4> data = {1.0f, 2.0f, 3.0f, 4.0f};
    BufferView view(data.data(), 4);

    const float* rp = view.get_read_pointer(0);
    EXPECT_FLOAT_EQ(rp[0], 1.0f);
    EXPECT_FLOAT_EQ(rp[3], 4.0f);
}

TEST(BufferView, MultiChannelConstructor) {
    std::array<float, 4> ch0 = {1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> ch1 = {5.0f, 6.0f, 7.0f, 8.0f};
    std::array<float*, 2> channels = {ch0.data(), ch1.data()};

    BufferView view(channels.data(), 2, 4);

    EXPECT_EQ(view.get_num_channels(), 2u);
    EXPECT_EQ(view.get_num_samples(), 4u);
    EXPECT_EQ(view.get_write_pointer(0), ch0.data());
    EXPECT_EQ(view.get_write_pointer(1), ch1.data());
}

TEST(BufferView, FromBuffer) {
    BufferF buffer(2, 64);
    buffer.set_sample(0, 10, 0.5f);

    BufferView view(buffer);

    EXPECT_EQ(view.get_num_channels(), 2u);
    EXPECT_EQ(view.get_num_samples(), 64u);
    EXPECT_FLOAT_EQ(view.get_read_pointer(0)[10], 0.5f);
}

TEST(ConstBufferView, FromConstBuffer) {
    BufferF buffer(2, 64);
    buffer.set_sample(1, 20, -0.75f);

    const BufferF& cref = buffer;
    ConstBufferView view(cref);

    EXPECT_EQ(view.get_num_channels(), 2u);
    EXPECT_EQ(view.get_num_samples(), 64u);
    EXPECT_FLOAT_EQ(view.get_read_pointer(1)[20], -0.75f);
}

TEST(ConstBufferView, MonoConstPointer) {
    const std::array<float, 4> data = {10.0f, 20.0f, 30.0f, 40.0f};
    ConstBufferView view(data.data(), 4);

    EXPECT_EQ(view.get_num_channels(), 1u);
    EXPECT_EQ(view.get_num_samples(), 4u);
    EXPECT_FLOAT_EQ(view.get_read_pointer(0)[2], 30.0f);
}

TEST(BufferView, ImplicitConversionToConst) {
    std::array<float, 4> data = {1.0f, 2.0f, 3.0f, 4.0f};
    BufferView mutable_view(data.data(), 4);

    ConstBufferView const_view = mutable_view;

    EXPECT_EQ(const_view.get_num_channels(), 1u);
    EXPECT_EQ(const_view.get_num_samples(), 4u);
    EXPECT_FLOAT_EQ(const_view.get_read_pointer(0)[0], 1.0f);
}

TEST(BufferView, ImplicitConversionMultiChannel) {
    std::array<float, 4> ch0 = {1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> ch1 = {5.0f, 6.0f, 7.0f, 8.0f};
    std::array<float*, 2> channels = {ch0.data(), ch1.data()};

    BufferView mutable_view(channels.data(), 2, 4);
    ConstBufferView const_view = mutable_view;

    EXPECT_EQ(const_view.get_num_channels(), 2u);
    EXPECT_FLOAT_EQ(const_view.get_read_pointer(1)[0], 5.0f);
}

TEST(BufferView, CopyMonoFixup) {
    std::array<float, 8> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    BufferView original(data.data(), 8);

    BufferView copy(original);

    EXPECT_EQ(copy.get_num_channels(), 1u);
    EXPECT_EQ(copy.get_num_samples(), 8u);
    EXPECT_EQ(copy.get_write_pointer(0), data.data());
    EXPECT_FLOAT_EQ(copy.get_read_pointer(0)[7], 8.0f);
}

TEST(BufferView, MoveMonoFixup) {
    std::array<float, 8> data = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    BufferView original(data.data(), 8);

    BufferView moved(std::move(original));

    EXPECT_EQ(moved.get_num_channels(), 1u);
    EXPECT_EQ(moved.get_num_samples(), 8u);
    EXPECT_EQ(moved.get_write_pointer(0), data.data());
    EXPECT_FLOAT_EQ(moved.get_read_pointer(0)[7], 8.0f);
}

TEST(BufferView, AssignMonoFixup) {
    std::array<float, 4> data_a = {1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> data_b = {5.0f, 6.0f, 7.0f, 8.0f};
    BufferView a(data_a.data(), 4);
    BufferView b(data_b.data(), 4);

    b = a;

    EXPECT_EQ(b.get_num_channels(), 1u);
    EXPECT_EQ(b.get_write_pointer(0), data_a.data());
    EXPECT_FLOAT_EQ(b.get_read_pointer(0)[0], 1.0f);
}

TEST(BufferView, SpanAccess) {
    std::array<float, 4> data = {10.0f, 20.0f, 30.0f, 40.0f};
    BufferView view(data.data(), 4);

    std::span<float> ch0 = view[0];
    EXPECT_EQ(ch0.size(), 4u);
    EXPECT_FLOAT_EQ(ch0[0], 10.0f);
    EXPECT_FLOAT_EQ(ch0[3], 40.0f);

    ch0[0] = 99.0f;
    EXPECT_FLOAT_EQ(data[0], 99.0f);
}

TEST(BufferView, ConstSpanAccess) {
    std::array<float, 4> data = {10.0f, 20.0f, 30.0f, 40.0f};
    const BufferView view(data.data(), 4);

    std::span<const float> ch0 = view[0];
    EXPECT_EQ(ch0.size(), 4u);
    EXPECT_FLOAT_EQ(ch0[2], 30.0f);
}

TEST(BufferView, MoveAssignMonoFixup) {
    std::array<float, 4> data_a = {1.0f, 2.0f, 3.0f, 4.0f};
    std::array<float, 4> data_b = {5.0f, 6.0f, 7.0f, 8.0f};
    BufferView a(data_a.data(), 4);
    BufferView b(data_b.data(), 4);

    b = std::move(a);

    EXPECT_EQ(b.get_num_channels(), 1u);
    EXPECT_EQ(b.get_write_pointer(0), data_a.data());
    EXPECT_FLOAT_EQ(b.get_read_pointer(0)[0], 1.0f);
}

TEST(BufferView, BufferViewIntegration) {
    Buffer<float> buffer(2, 4);
    buffer.set_sample(0, 0, 1.0f);
    buffer.set_sample(0, 3, 2.0f);
    buffer.set_sample(1, 1, 3.0f);

    BufferView view = buffer.view();
    EXPECT_EQ(view.get_num_channels(), 2u);
    EXPECT_EQ(view.get_num_samples(), 4u);
    EXPECT_FLOAT_EQ(view.get_read_pointer(0)[0], 1.0f);
    EXPECT_FLOAT_EQ(view.get_read_pointer(0)[3], 2.0f);
    EXPECT_FLOAT_EQ(view.get_read_pointer(1)[1], 3.0f);

    const Buffer<float>& cbuffer = buffer;
    ConstBufferView const_view = cbuffer.view();
    EXPECT_EQ(const_view.get_num_channels(), 2u);
    EXPECT_EQ(const_view.get_num_samples(), 4u);
    EXPECT_FLOAT_EQ(const_view.get_read_pointer(0)[0], 1.0f);
    EXPECT_FLOAT_EQ(const_view.get_read_pointer(1)[1], 3.0f);
}

TEST(BufferView, SubBlockMono) {
    std::vector<float> data(512, 0.0f);
    for (size_t i = 0; i < data.size(); ++i) { data[i] = static_cast<float>(i); }

    BufferView view(data.data(), 512);

    auto sub = view.sub_block(100, 50);
    EXPECT_EQ(sub.get_num_samples(), 50u);
    EXPECT_EQ(sub.get_num_channels(), 1u);

    // Verify the sub_block points to the right data
    EXPECT_FLOAT_EQ(sub.get_read_pointer(0)[0], 100.0f);
    EXPECT_FLOAT_EQ(sub.get_read_pointer(0)[49], 149.0f);
}

TEST(BufferView, SubBlockStereo) {
    std::vector<float> left(512), right(512);
    for (size_t i = 0; i < 512; ++i) {
        left[i] = static_cast<float>(i);
        right[i] = static_cast<float>(i + 1000);
    }

    std::array<float*, 2> channels = {left.data(), right.data()};
    BufferView view(channels.data(), 2, 512);

    auto sub = view.sub_block(200, 100);
    EXPECT_EQ(sub.get_num_samples(), 100u);
    EXPECT_EQ(sub.get_num_channels(), 2u);

    EXPECT_FLOAT_EQ(sub.get_read_pointer(0)[0], 200.0f);
    EXPECT_FLOAT_EQ(sub.get_read_pointer(0)[99], 299.0f);
    EXPECT_FLOAT_EQ(sub.get_read_pointer(1)[0], 1200.0f);
    EXPECT_FLOAT_EQ(sub.get_read_pointer(1)[99], 1299.0f);
}

TEST(BufferView, SubBlockSpan) {
    std::vector<float> data(512, 0.0f);
    for (size_t i = 0; i < data.size(); ++i) { data[i] = static_cast<float>(i); }

    BufferView view(data.data(), 512);
    auto sub = view.sub_block(10, 20);

    auto span = sub[0];
    EXPECT_EQ(span.size(), 20u);
    EXPECT_FLOAT_EQ(span[0], 10.0f);
    EXPECT_FLOAT_EQ(span[19], 29.0f);
}

TEST(BufferView, SubBlockChained) {
    std::vector<float> data(512, 0.0f);
    for (size_t i = 0; i < data.size(); ++i) { data[i] = static_cast<float>(i); }

    BufferView view(data.data(), 512);
    auto sub1 = view.sub_block(100, 200);
    auto sub2 = sub1.sub_block(50, 50);

    EXPECT_EQ(sub2.get_num_samples(), 50u);
    EXPECT_FLOAT_EQ(sub2.get_read_pointer(0)[0], 150.0f);
    EXPECT_FLOAT_EQ(sub2.get_read_pointer(0)[49], 199.0f);
}
