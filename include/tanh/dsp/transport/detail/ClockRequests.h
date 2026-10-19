#pragma once

#include <tanh/utils/RealtimeSanitizer.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace thl::dsp::transport::detail {

/// A time signature set from any thread and read whole on the audio thread.
class AtomicTimeSignature {
public:
    struct Value {
        int m_num = 4;
        int m_denom = 4;
    };

    /// Ignores a numerator or denominator <= 0.
    void store(int num, int denom) TANH_NONBLOCKING_FUNCTION {
        if (num <= 0 || denom <= 0) { return; }
        m_packed.store(pack(num, denom), std::memory_order_relaxed);
    }

    [[nodiscard]] Value load() const TANH_NONBLOCKING_FUNCTION {
        const uint64_t p = m_packed.load(std::memory_order_relaxed);
        return {.m_num = static_cast<int>(p >> 32U), .m_denom = static_cast<int>(p & 0xffffffffU)};
    }

private:
    static constexpr uint64_t pack(int num, int denom) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(num)) << 32U) |
               static_cast<uint32_t>(denom);
    }

    std::atomic<uint64_t> m_packed{pack(4, 4)};
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
};

/// A value requested from any thread and taken once on the audio thread; the
/// latest request wins.
class PendingValue {
public:
    /// Ignores a non-finite value.
    void request(double value) TANH_NONBLOCKING_FUNCTION {
        if (!std::isfinite(value)) { return; }
        m_value.store(value, std::memory_order_release);
    }

    /// The request since the last take(), if any.
    [[nodiscard]] std::optional<double> take() TANH_NONBLOCKING_FUNCTION {
        const double v = m_value.exchange(k_none, std::memory_order_acq_rel);
        if (std::isnan(v)) { return std::nullopt; }
        return v;
    }

private:
    static constexpr double k_none = std::numeric_limits<double>::quiet_NaN();

    std::atomic<double> m_value{k_none};
    static_assert(std::atomic<double>::is_always_lock_free);
};

}  // namespace thl::dsp::transport::detail
