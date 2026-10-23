#include <gtest/gtest.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/BaseProcessor.h>

#include <span>
#include <vector>

#include "TestHelpers.h"

// Test processor that counts how many times process() and process_dry() are called
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
        m_block_sizes.reserve(samples_per_block);
    }

    void process(thl::core::BufferView buffer, uint32_t /*modulation_offset*/ = 0) override {
        m_block_sizes.push_back(buffer.get_num_samples());
    }

    void process_dry(thl::core::BufferView buffer, uint32_t modulation_offset = 0) override {
        m_dry_block_sizes.push_back(buffer.get_num_samples());
        m_dry_offsets.push_back(modulation_offset);
    }

protected:
    std::span<const uint32_t> get_change_points() override { return m_change_points; }
};

TEST(BaseProcessor, ProcessModulatedNoChangePoints) {
    CallCountingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> data(k_block_size, 1.0f);
    thl::core::BufferView view(data.data(), k_block_size);

    proc.process_modulated(view, {});
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

TEST(BaseProcessor, ProcessDryModulatedSplitsLikeProcessModulated) {
    CallCountingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> data(512, 1.0f);
    thl::core::BufferView view(data.data(), 512);

    std::vector<uint32_t> cps = {100, 300};
    proc.process_modulated(view, std::span<const uint32_t>(cps));
    proc.process_dry_modulated(view, std::span<const uint32_t>(cps));

    EXPECT_EQ(proc.m_dry_block_sizes, proc.m_block_sizes);
    EXPECT_EQ(proc.m_dry_offsets, (std::vector<uint32_t>{0, 100, 300}));
}

TEST(BaseProcessor, ProcessDryModulatedUsesGetChangePoints) {
    CallCountingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);
    proc.m_change_points = {256};

    std::vector<float> data(512, 1.0f);
    thl::core::BufferView view(data.data(), 512);

    proc.process_dry_modulated(view);

    EXPECT_EQ(proc.m_dry_block_sizes, (std::vector<size_t>{256, 256}));
    EXPECT_EQ(proc.m_dry_offsets, (std::vector<uint32_t>{0, 256}));
    EXPECT_TRUE(proc.m_block_sizes.empty());
}
