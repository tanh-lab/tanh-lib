#include <gtest/gtest.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/BaseProcessor.h>

#include <span>
#include <utility>
#include <vector>

#include "TestHelpers.h"

// Test processor that counts how many times process() is called
// and records the buffer sizes.
class CallCountingProcessor : public thl::dsp::BaseProcessor {
public:
    std::vector<size_t> m_block_sizes;
    std::vector<size_t> m_dry_block_sizes;
    std::vector<uint32_t> m_dry_offsets;
    std::vector<uint32_t> m_change_points;

    void prepare(const double& /*sample_rate*/,
                 const size_t& samples_per_block,
                 const size_t& /*num_channels*/) override {
        // process() runs under RTSan: record without allocating.
        m_block_sizes.reserve(samples_per_block);
        m_dry_block_sizes.reserve(samples_per_block);
        m_dry_offsets.reserve(samples_per_block);
    }

    void process(thl::core::BufferView buffer, uint32_t /*modulation_offset*/ = 0) override {
        m_block_sizes.push_back(buffer.get_num_samples());
    }

    void process(thl::core::BufferView /*buffer*/,
                 uint32_t modulation_offset,
                 thl::core::BufferView dry) override {
        m_dry_block_sizes.push_back(dry.get_num_samples());
        m_dry_offsets.push_back(modulation_offset);
    }

protected:
    std::span<const uint32_t> get_change_points() override { return m_change_points; }
};

// Test processor that doubles the signal and leaves the dry signal to the default process().
class DoublingProcessor : public thl::dsp::BaseProcessor {
public:
    void prepare(const double& /*sample_rate*/,
                 const size_t& /*samples_per_block*/,
                 const size_t& /*num_channels*/) override {}

    void process(thl::core::BufferView buffer, uint32_t /*modulation_offset*/ = 0) override {
        for (size_t channel = 0; channel < buffer.get_num_channels(); ++channel) {
            float* samples = buffer.get_write_pointer(channel);
            for (size_t n = 0; n < buffer.get_num_samples(); ++n) { samples[n] *= 2.0f; }
        }
    }
};

TEST(BaseProcessor, ProcessModulatedNoChangePoints) {
    CallCountingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> data(k_block_size, 1.0f);
    thl::core::BufferView view(data.data(), k_block_size);

    proc.process_modulated(view, std::span<const uint32_t>());
    ASSERT_EQ(proc.m_block_sizes.size(), 1u);
    EXPECT_EQ(proc.m_block_sizes[0], 512u);
}

TEST(BaseProcessor, ProcessModulatedWithChangePoints) {
    CallCountingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> data(512, 1.0f);
    thl::core::BufferView view(data.data(), 512);

    std::vector<uint32_t> cps = {100, 300};
    proc.process_modulated(view, std::span<const uint32_t>(cps));

    // Should split into 3 blocks: [0,100), [100,300), [300,512)
    ASSERT_EQ(proc.m_block_sizes.size(), 3u);
    EXPECT_EQ(proc.m_block_sizes[0], 100u);
    EXPECT_EQ(proc.m_block_sizes[1], 200u);
    EXPECT_EQ(proc.m_block_sizes[2], 212u);
}

TEST(BaseProcessor, ProcessModulatedSkipsInvalidChangePoints) {
    CallCountingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> data(512, 1.0f);
    thl::core::BufferView view(data.data(), 512);

    // Include edge cases: 0 (at start, skip), 512 (at end, skip), duplicate
    std::vector<uint32_t> cps = {0, 200, 200, 512};
    proc.process_modulated(view, std::span<const uint32_t>(cps));

    // 0 is skipped (pos starts at 0, cp <= pos), 200 first time splits,
    // 200 second time skipped (cp <= pos), 512 skipped (cp >= total)
    // Result: [0,200), [200,512)
    ASSERT_EQ(proc.m_block_sizes.size(), 2u);
    EXPECT_EQ(proc.m_block_sizes[0], 200u);
    EXPECT_EQ(proc.m_block_sizes[1], 312u);
}

TEST(BaseProcessor, ProcessModulatedWithDrySplitsLikeWithout) {
    CallCountingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> data(512, 1.0f);
    std::vector<float> dry_data(512, 0.0f);
    thl::core::BufferView view(data.data(), 512);
    thl::core::BufferView dry(dry_data.data(), 512);

    std::vector<uint32_t> cps = {100, 300};
    proc.process_modulated(view, std::span<const uint32_t>(cps));
    proc.process_modulated(view, std::span<const uint32_t>(cps), dry);

    EXPECT_EQ(proc.m_dry_block_sizes, proc.m_block_sizes);
    EXPECT_EQ(proc.m_dry_offsets, (std::vector<uint32_t>{0, 100, 300}));
}

TEST(BaseProcessor, ProcessModulatedWithDryUsesGetChangePoints) {
    CallCountingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);
    proc.m_change_points = {256};

    std::vector<float> data(512, 1.0f);
    std::vector<float> dry_data(512, 0.0f);
    thl::core::BufferView view(data.data(), 512);
    thl::core::BufferView dry(dry_data.data(), 512);

    proc.process_modulated(view, dry);

    EXPECT_EQ(proc.m_dry_block_sizes, (std::vector<size_t>{256, 256}));
    EXPECT_EQ(proc.m_dry_offsets, (std::vector<uint32_t>{0, 256}));
    EXPECT_TRUE(proc.m_block_sizes.empty());
}

TEST(BaseProcessor, DefaultProcessWithDryCopiesTheInput) {
    DoublingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> data(512);
    for (size_t n = 0; n < data.size(); ++n) { data[n] = static_cast<float>(n); }
    const std::vector<float> input = data;
    std::vector<float> dry_data(512, 0.0f);
    thl::core::BufferView view(data.data(), 512);
    thl::core::BufferView dry(dry_data.data(), 512);

    std::vector<uint32_t> cps = {100, 300};
    proc.process_modulated(view, std::span<const uint32_t>(cps), dry);

    for (size_t n = 0; n < data.size(); ++n) {
        EXPECT_FLOAT_EQ(dry_data[n], input[n]);
        EXPECT_FLOAT_EQ(data[n], 2.0f * input[n]);
    }
}

// A buffer that starts at a block position: change points stay block positions,
// each part gets its block position as the modulation offset.
TEST(BaseProcessor, ProcessModulatedAtAnOffsetSplitsAtBlockPositions) {
    class OffsetRecorder : public thl::dsp::BaseProcessor {
    public:
        std::vector<std::pair<uint32_t, size_t>> m_calls;  // reserved: process() runs under RTSan
        void prepare(const double&, const size_t&, const size_t&) override {}
        void process(thl::core::BufferView buffer, uint32_t modulation_offset) override {
            m_calls.emplace_back(modulation_offset, buffer.get_num_samples());
        }
    };
    OffsetRecorder proc;
    proc.m_calls.reserve(8);
    std::vector<float> audio(64, 0.0f);
    thl::core::BufferView view(audio.data(), 64);
    const std::vector<uint32_t> cps = {16, 40, 48, 100};  // 16 before, 100 after the part

    proc.process_modulated(view.sub_block(32, 32), std::span<const uint32_t>(cps), 32);
    using Calls = std::vector<std::pair<uint32_t, size_t>>;
    EXPECT_EQ(proc.m_calls, (Calls{{32, 8}, {40, 8}, {48, 16}}));
}

// With a dry buffer at a block position: both are split at the same block positions.
TEST(BaseProcessor, ProcessModulatedWithDryAtAnOffsetSplitsBothAtBlockPositions) {
    CallCountingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);
    std::vector<float> audio(64, 0.0f);
    std::vector<float> dry_audio(64, 0.0f);
    thl::core::BufferView view(audio.data(), 64);
    thl::core::BufferView dry(dry_audio.data(), 64);
    const std::vector<uint32_t> cps = {16, 40, 48, 100};

    proc.process_modulated(view.sub_block(32, 32),
                           std::span<const uint32_t>(cps),
                           dry.sub_block(32, 32),
                           32);
    EXPECT_EQ(proc.m_dry_offsets, (std::vector<uint32_t>{32, 40, 48}));
    EXPECT_EQ(proc.m_dry_block_sizes, (std::vector<size_t>{8, 8, 16}));
}
