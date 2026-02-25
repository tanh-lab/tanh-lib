#include <tanh/dsp/utils/DspMath.h>

#include <cmath>

namespace thl::dsp::utils {

float SemitonesToRatio(float semitones) {
    // Initial implementation prioritizes correctness and simplicity.
    // Can be replaced with the stmlib LUT port later if profiling warrants it.
    return std::exp2(semitones / 12.0f);
}

} // namespace thl::dsp::utils

