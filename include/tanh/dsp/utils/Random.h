#pragma once

#include <cstdint>

namespace thl::dsp::utils {

class Random {
public:
    static uint32_t state() { return rng_state_; }

    static void Seed(uint16_t seed) {
        rng_state_ = seed;
    }

    static uint32_t GetWord() {
        rng_state_ = rng_state_ * 1664525u + 1013904223u;
        return rng_state_;
    }

    static int16_t GetSample() {
        return static_cast<int16_t>(GetWord() >> 16);
    }

    static float GetFloat() {
        return static_cast<float>(GetWord()) / 4294967296.0f;
    }

    static uint32_t GetGeometric(uint16_t p) {
        const uint16_t one_minus_p = static_cast<uint16_t>(0x10000u - p);
        const uint32_t log_u = nlog2_16(static_cast<uint16_t>(GetWord() >> 16));
        const uint32_t log_p = nlog2_16(one_minus_p);
        return log_u / log_p;
    }

private:
    static uint32_t rng_state_;
    static uint32_t nlog2_16(uint16_t x);

    Random() = delete;
    Random(const Random&) = delete;
    Random& operator=(const Random&) = delete;
};

} // namespace thl::dsp::utils

