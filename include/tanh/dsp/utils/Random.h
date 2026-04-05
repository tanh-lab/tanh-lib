#pragma once

#include <cstdint>

#include <tanh/core/Exports.h>

namespace thl::dsp::utils {

class TANH_API Random {
public:
    static uint32_t state() { return m_rng_state; }

    static void seed(uint16_t seed) { m_rng_state = seed; }

    static uint32_t get_word() {
        m_rng_state = m_rng_state * 1664525u + 1013904223u;
        return m_rng_state;
    }

    static int16_t get_sample() { return static_cast<int16_t>(get_word() >> 16); }

    static float get_float() { return static_cast<float>(get_word()) / 4294967296.0f; }

    static uint32_t get_geometric(uint16_t p) {
        const auto one_minus_p = static_cast<uint16_t>(0x10000u - p);
        const uint32_t log_u = nlog2_16(static_cast<uint16_t>(get_word() >> 16));
        const uint32_t log_p = nlog2_16(one_minus_p);
        return log_u / log_p;
    }

    Random() = delete;
    Random(const Random&) = delete;
    Random& operator=(const Random&) = delete;

private:
    // inline static avoids cross-DLL symbol dependency on MSVC (each DLL gets its own state copy)
    static inline uint32_t m_rng_state = 0x21u;
    static uint32_t nlog2_16(uint16_t x);
};

}  // namespace thl::dsp::utils
