#include <gtest/gtest.h>

#include <span>
#include <vector>

#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/audio/AudioBufferView.h>

#include "TestHelpers.h"

// Test processor that counts how many times process() is called
// and records the buffer sizes.
class CallCountingProcessor : public thl::dsp::BaseProcessor {
public:
    std::vector<size_t> m_block_sizes;

    void prepare(const double& /*sample_rate*/,
                 const size_t& samples_per_block,
                 const size_t& /*num_channels*/) override {
        m_block_sizes.reserve(samples_per_block);
    }

    void process(thl::dsp::audio::AudioBufferView buffer,
                 uint32_t /*modulation_offset*/ = 0) override {
        m_block_sizes.push_back(buffer.get_num_frames());
    }
};

TEST(BaseProcessor, ProcessModulatedNoChangePoints) {
    CallCountingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> data(k_block_size, 1.0f);
    thl::dsp::audio::AudioBufferView view(data.data(), k_block_size);

    proc.process_modulated(view, {});
    ASSERT_EQ(proc.m_block_sizes.size(), 1u);
    EXPECT_EQ(proc.m_block_sizes[0], 512u);
}

TEST(BaseProcessor, ProcessModulatedWithChangePoints) {
    CallCountingProcessor proc;
    proc.prepare(k_sample_rate, k_block_size, 1);

    std::vector<float> data(512, 1.0f);
    thl::dsp::audio::AudioBufferView view(data.data(), 512);

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
    thl::dsp::audio::AudioBufferView view(data.data(), 512);

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
