#pragma once

#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstdint>
#include <span>

namespace thl::dsp {

class TANH_API BaseProcessor {
public:
    virtual ~BaseProcessor() = default;

    virtual void prepare(const double& sample_rate,
                         const size_t& samples_per_block,
                         const size_t& num_channels) = 0;
    virtual void process(thl::core::BufferView buffer,
                         uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION = 0;

    /// process() with phase compensation on dry signal. Default copies
    /// @p buffer into @p dry. Override it if process() changes the signal phase.
    virtual void process(thl::core::BufferView buffer,
                         uint32_t modulation_offset,
                         thl::core::BufferView dry) TANH_NONBLOCKING_FUNCTION;

    // Self-driven: calls get_change_points() to obtain split positions
    void process_modulated(const thl::core::BufferView& buffer) TANH_NONBLOCKING_FUNCTION;
    void process_modulated(const thl::core::BufferView& buffer,
                           const thl::core::BufferView& dry) TANH_NONBLOCKING_FUNCTION;

    // Caller-injected: uses externally supplied change points
    void process_modulated(const thl::core::BufferView& buffer,
                           std::span<const uint32_t> change_points) TANH_NONBLOCKING_FUNCTION;
    void process_modulated(const thl::core::BufferView& buffer,
                           std::span<const uint32_t> change_points,
                           const thl::core::BufferView& dry) TANH_NONBLOCKING_FUNCTION;

protected:
    virtual std::span<const uint32_t> get_change_points() TANH_NONBLOCKING_FUNCTION { return {}; }

private:
    void split_and_process(const thl::core::BufferView& buffer,
                           std::span<const uint32_t> change_points,
                           const thl::core::BufferView& dry = {}) TANH_NONBLOCKING_FUNCTION;
};

}  // namespace thl::dsp
