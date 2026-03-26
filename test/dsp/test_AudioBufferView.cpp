#include <gtest/gtest.h>
#include "tanh/dsp/audio/AudioBuffer.h"

#include <utility>
#include <vector>

using namespace thl::dsp::audio;

TEST(AudioBufferView, MonoConstructor) {
    float data[8] = {};
    AudioBufferView view(data, 8);

    EXPECT_EQ(view.get_num_channels(), 1u);
    EXPECT_EQ(view.get_num_frames(), 8u);
    EXPECT_EQ(view.get_write_pointer(0), data);
}

TEST(AudioBufferView, MonoConstructorReadPointer) {
    float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    AudioBufferView view(data, 4);

    const float* rp = view.get_read_pointer(0);
    EXPECT_FLOAT_EQ(rp[0], 1.0f);
    EXPECT_FLOAT_EQ(rp[3], 4.0f);
}

TEST(AudioBufferView, MultiChannelConstructor) {
    float ch0[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float ch1[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    float* channels[] = {ch0, ch1};

    AudioBufferView view(channels, 2, 4);

    EXPECT_EQ(view.get_num_channels(), 2u);
    EXPECT_EQ(view.get_num_frames(), 4u);
    EXPECT_EQ(view.get_write_pointer(0), ch0);
    EXPECT_EQ(view.get_write_pointer(1), ch1);
}

TEST(AudioBufferView, FromBuffer) {
    AudioBuffer buffer(2, 64);
    buffer.set_sample(0, 10, 0.5f);

    AudioBufferView view(buffer);

    EXPECT_EQ(view.get_num_channels(), 2u);
    EXPECT_EQ(view.get_num_frames(), 64u);
    EXPECT_FLOAT_EQ(view.get_read_pointer(0)[10], 0.5f);
}

TEST(ConstAudioBufferView, FromConstBuffer) {
    AudioBuffer buffer(2, 64);
    buffer.set_sample(1, 20, -0.75f);

    const AudioBuffer& cref = buffer;
    ConstAudioBufferView view(cref);

    EXPECT_EQ(view.get_num_channels(), 2u);
    EXPECT_EQ(view.get_num_frames(), 64u);
    EXPECT_FLOAT_EQ(view.get_read_pointer(1)[20], -0.75f);
}

TEST(ConstAudioBufferView, MonoConstPointer) {
    const float data[4] = {10.0f, 20.0f, 30.0f, 40.0f};
    ConstAudioBufferView view(data, 4);

    EXPECT_EQ(view.get_num_channels(), 1u);
    EXPECT_EQ(view.get_num_frames(), 4u);
    EXPECT_FLOAT_EQ(view.get_read_pointer(0)[2], 30.0f);
}

TEST(AudioBufferView, ImplicitConversionToConst) {
    float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    AudioBufferView mutable_view(data, 4);

    ConstAudioBufferView const_view = mutable_view;

    EXPECT_EQ(const_view.get_num_channels(), 1u);
    EXPECT_EQ(const_view.get_num_frames(), 4u);
    EXPECT_FLOAT_EQ(const_view.get_read_pointer(0)[0], 1.0f);
}

TEST(AudioBufferView, ImplicitConversionMultiChannel) {
    float ch0[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float ch1[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    float* channels[] = {ch0, ch1};

    AudioBufferView mutable_view(channels, 2, 4);
    ConstAudioBufferView const_view = mutable_view;

    EXPECT_EQ(const_view.get_num_channels(), 2u);
    EXPECT_FLOAT_EQ(const_view.get_read_pointer(1)[0], 5.0f);
}

TEST(AudioBufferView, CopyMonoFixup) {
    float data[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    AudioBufferView original(data, 8);

    AudioBufferView copy(original);

    EXPECT_EQ(copy.get_num_channels(), 1u);
    EXPECT_EQ(copy.get_num_frames(), 8u);
    EXPECT_EQ(copy.get_write_pointer(0), data);
    EXPECT_FLOAT_EQ(copy.get_read_pointer(0)[7], 8.0f);
}

TEST(AudioBufferView, MoveMonoFixup) {
    float data[8] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    AudioBufferView original(data, 8);

    AudioBufferView moved(std::move(original));

    EXPECT_EQ(moved.get_num_channels(), 1u);
    EXPECT_EQ(moved.get_num_frames(), 8u);
    EXPECT_EQ(moved.get_write_pointer(0), data);
    EXPECT_FLOAT_EQ(moved.get_read_pointer(0)[7], 8.0f);
}

TEST(AudioBufferView, AssignMonoFixup) {
    float data_a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float data_b[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    AudioBufferView a(data_a, 4);
    AudioBufferView b(data_b, 4);

    b = a;

    EXPECT_EQ(b.get_num_channels(), 1u);
    EXPECT_EQ(b.get_write_pointer(0), data_a);
    EXPECT_FLOAT_EQ(b.get_read_pointer(0)[0], 1.0f);
}

TEST(AudioBufferView, SpanAccess) {
    float data[4] = {10.0f, 20.0f, 30.0f, 40.0f};
    AudioBufferView view(data, 4);

    std::span<float> ch0 = view[0];
    EXPECT_EQ(ch0.size(), 4u);
    EXPECT_FLOAT_EQ(ch0[0], 10.0f);
    EXPECT_FLOAT_EQ(ch0[3], 40.0f);

    ch0[0] = 99.0f;
    EXPECT_FLOAT_EQ(data[0], 99.0f);
}

TEST(AudioBufferView, ConstSpanAccess) {
    float data[4] = {10.0f, 20.0f, 30.0f, 40.0f};
    const AudioBufferView view(data, 4);

    std::span<const float> ch0 = view[0];
    EXPECT_EQ(ch0.size(), 4u);
    EXPECT_FLOAT_EQ(ch0[2], 30.0f);
}

TEST(AudioBufferView, MoveAssignMonoFixup) {
    float data_a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float data_b[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    AudioBufferView a(data_a, 4);
    AudioBufferView b(data_b, 4);

    b = std::move(a);

    EXPECT_EQ(b.get_num_channels(), 1u);
    EXPECT_EQ(b.get_write_pointer(0), data_a);
    EXPECT_FLOAT_EQ(b.get_read_pointer(0)[0], 1.0f);
}

TEST(AudioBufferView, BufferViewIntegration) {
    Buffer<float> buffer(2, 4);
    buffer.set_sample(0, 0, 1.0f);
    buffer.set_sample(0, 3, 2.0f);
    buffer.set_sample(1, 1, 3.0f);

    AudioBufferView view = buffer.view();
    EXPECT_EQ(view.get_num_channels(), 2u);
    EXPECT_EQ(view.get_num_frames(), 4u);
    EXPECT_FLOAT_EQ(view.get_read_pointer(0)[0], 1.0f);
    EXPECT_FLOAT_EQ(view.get_read_pointer(0)[3], 2.0f);
    EXPECT_FLOAT_EQ(view.get_read_pointer(1)[1], 3.0f);

    const Buffer<float>& cbuffer = buffer;
    ConstAudioBufferView const_view = cbuffer.view();
    EXPECT_EQ(const_view.get_num_channels(), 2u);
    EXPECT_EQ(const_view.get_num_frames(), 4u);
    EXPECT_FLOAT_EQ(const_view.get_read_pointer(0)[0], 1.0f);
    EXPECT_FLOAT_EQ(const_view.get_read_pointer(1)[1], 3.0f);
}

TEST(AudioBufferView, SubBlockMono) {
    std::vector<float> data(512, 0.0f);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<float>(i);
    }

    AudioBufferView view(data.data(), 512);

    auto sub = view.sub_block(100, 50);
    EXPECT_EQ(sub.get_num_frames(), 50u);
    EXPECT_EQ(sub.get_num_channels(), 1u);

    // Verify the sub_block points to the right data
    EXPECT_FLOAT_EQ(sub.get_read_pointer(0)[0], 100.0f);
    EXPECT_FLOAT_EQ(sub.get_read_pointer(0)[49], 149.0f);
}

TEST(AudioBufferView, SubBlockStereo) {
    std::vector<float> left(512), right(512);
    for (size_t i = 0; i < 512; ++i) {
        left[i] = static_cast<float>(i);
        right[i] = static_cast<float>(i + 1000);
    }

    float* channels[] = {left.data(), right.data()};
    AudioBufferView view(channels, 2, 512);

    auto sub = view.sub_block(200, 100);
    EXPECT_EQ(sub.get_num_frames(), 100u);
    EXPECT_EQ(sub.get_num_channels(), 2u);

    EXPECT_FLOAT_EQ(sub.get_read_pointer(0)[0], 200.0f);
    EXPECT_FLOAT_EQ(sub.get_read_pointer(0)[99], 299.0f);
    EXPECT_FLOAT_EQ(sub.get_read_pointer(1)[0], 1200.0f);
    EXPECT_FLOAT_EQ(sub.get_read_pointer(1)[99], 1299.0f);
}

TEST(AudioBufferView, SubBlockSpan) {
    std::vector<float> data(512, 0.0f);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<float>(i);
    }

    AudioBufferView view(data.data(), 512);
    auto sub = view.sub_block(10, 20);

    auto span = sub[0];
    EXPECT_EQ(span.size(), 20u);
    EXPECT_FLOAT_EQ(span[0], 10.0f);
    EXPECT_FLOAT_EQ(span[19], 29.0f);
}

TEST(AudioBufferView, SubBlockChained) {
    std::vector<float> data(512, 0.0f);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<float>(i);
    }

    AudioBufferView view(data.data(), 512);
    auto sub1 = view.sub_block(100, 200);
    auto sub2 = sub1.sub_block(50, 50);

    EXPECT_EQ(sub2.get_num_frames(), 50u);
    EXPECT_FLOAT_EQ(sub2.get_read_pointer(0)[0], 150.0f);
    EXPECT_FLOAT_EQ(sub2.get_read_pointer(0)[49], 199.0f);
}
