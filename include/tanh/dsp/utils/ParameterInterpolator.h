#pragma once

#include <cstddef>

namespace thl::dsp::utils {

class ParameterInterpolator {
public:
    ParameterInterpolator(float* state, float new_value, size_t size)
        : state_(state),
          value_(*state),
          increment_(size == 0 ? 0.0f : (new_value - *state) / static_cast<float>(size)) {}

    ~ParameterInterpolator() {
        *state_ = value_;
    }

    float Next() {
        value_ += increment_;
        return value_;
    }

    float subsample(float t) const {
        return value_ + increment_ * t;
    }

private:
    float* state_;
    float value_;
    float increment_;
};

} // namespace thl::dsp::utils

