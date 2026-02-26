#include <tanh/dsp/utils/DspMath.h>

#include <cmath>

namespace thl::dsp::utils {

float semitones_to_ratio(float semitones) {
    return std::exp2(semitones / 12.0f);
}

} // namespace thl::dsp::utils

