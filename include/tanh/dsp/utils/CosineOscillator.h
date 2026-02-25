#pragma once

#include <cmath>

namespace thl::dsp::utils {

enum class CosineOscillatorMode {
    Approximate,
    Exact,
};

class CosineOscillator {
public:
    template <CosineOscillatorMode mode>
    void Init(float frequency) {
        if constexpr (mode == CosineOscillatorMode::Approximate) {
            InitApproximate(frequency);
        } else {
            iir_coefficient_ = 2.0f * std::cos(2.0f * static_cast<float>(M_PI) * frequency);
            initial_amplitude_ = iir_coefficient_ * 0.25f;
        }
        Start();
    }

    void InitApproximate(float frequency) {
        float sign = 16.0f;
        frequency -= 0.25f;
        if (frequency < 0.0f) {
            frequency = -frequency;
        } else {
            if (frequency > 0.5f) {
                frequency -= 0.5f;
            } else {
                sign = -16.0f;
            }
        }
        iir_coefficient_ = sign * frequency * (1.0f - 2.0f * frequency);
        initial_amplitude_ = iir_coefficient_ * 0.25f;
    }

    void Start() {
        y1_ = initial_amplitude_;
        y0_ = 0.5f;
    }

    float value() const {
        return y1_ + 0.5f;
    }

    float Next() {
        const float temp = y0_;
        y0_ = iir_coefficient_ * y0_ - y1_;
        y1_ = temp;
        return temp + 0.5f;
    }

private:
    float y1_ = 0.0f;
    float y0_ = 0.5f;
    float iir_coefficient_ = 0.0f;
    float initial_amplitude_ = 0.0f;
};

} // namespace thl::dsp::utils

