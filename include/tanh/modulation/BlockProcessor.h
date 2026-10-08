#pragma once

#include <tanh/core/Exports.h>
#include <tanh/dsp/transport/TransportInfo.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>

namespace thl::modulation {

/// What ModulationMatrix knows about the block it processes.
struct BlockContext {
    size_t m_num_samples = 0;
    /// The block's musical time (m_num_samples frames from its m_beat_position), or nullptr
    /// when the caller passed none to ModulationMatrix::process().
    const dsp::transport::TransportInfo* m_transport = nullptr;
};

/**
 * @brief Work a ModulationMatrix runs once per block, before any source.
 *
 * For a component that renders several sources together or needs the block's
 * transport (XYController renders every dot's x, y and gate in one pass, synced
 * to the transport). Register it with ModulationMatrix::add_block_processor().
 *
 * Threading: prepare() on the message thread (from ModulationMatrix::prepare(),
 * or add_block_processor() after it); process_block() on the audio thread.
 */
class TANH_API BlockProcessor {
public:
    BlockProcessor() = default;
    virtual ~BlockProcessor() = default;

    BlockProcessor(const BlockProcessor&) = delete;
    BlockProcessor& operator=(const BlockProcessor&) = delete;
    BlockProcessor(BlockProcessor&&) = delete;
    BlockProcessor& operator=(BlockProcessor&&) = delete;

    /// Allocate for blocks of up to @p max_block_size samples.
    virtual void prepare(double sample_rate, size_t max_block_size) = 0;

    /// One block, after the sources' clear_per_block() and before their pre_process_block().
    virtual void process_block(const BlockContext& context) TANH_NONBLOCKING_FUNCTION = 0;
};

}  // namespace thl::modulation
