#pragma once

#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace thl::dsp::transport {

/**
 * @brief Maps an audio callback's sample time to a host time without the
 *        callback's own timestamp, as Ableton Link's HostTimeFilter does.
 *
 * Each block adds a point (sample time, host time read now) and the filter
 * returns the host time of the sample time on the linear regression over the
 * last k_points points, which removes the jitter of reading the clock inside
 * the callback. Fixed storage: real-time safe.
 */
class TANH_API HostTimeFilter {
public:
    /// Blocks the regression spans.
    static constexpr size_t k_points = 512;

    /// Forgets every point; @p sample_rate sets the slope used until there are two.
    void reset(double sample_rate) TANH_NONBLOCKING_FUNCTION;

    /// Adds the point (@p sample_time, @p now_us) and returns the filtered host
    /// time of @p sample_time in microseconds.
    [[nodiscard]] int64_t add(double sample_time, int64_t now_us) TANH_NONBLOCKING_FUNCTION;

private:
    std::array<double, k_points> m_x{};
    std::array<double, k_points> m_y{};
    size_t m_count = 0;
    size_t m_index = 0;
    double m_x0 = 0.0;  // offsets keep the sums small (precision)
    double m_y0 = 0.0;
    double m_default_slope = 1e6 / 48000.0;  // microseconds per sample
};

}  // namespace thl::dsp::transport
