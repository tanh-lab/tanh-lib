#pragma once

#include <cmath>

namespace thl::dsp::resonator {

class ParamSmoother {
public:
    void prepare(double sampleRate, float timeInSeconds) {
        coeff_ = std::exp(-1.0f / (static_cast<float>(sampleRate) * timeInSeconds));
    }

    void setTarget(float target) { target_ = target; }

    float skip(int numSamples) {
        current_ = target_ + (current_ - target_) * std::pow(coeff_, static_cast<float>(numSamples));
        return current_;
    }

    float getCurrentValue() const { return current_; }

private:
    float current_ = 0.0f;
    float target_ = 0.0f;
    float coeff_ = 0.0f;
};

} // namespace thl::dsp::resonator
