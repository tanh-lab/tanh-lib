#pragma once

#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/stmlib/dsp/dsp.h>

namespace stmlib {

inline float SemitonesToRatio(float semitones) {
    return thl::dsp::utils::SemitonesToRatio(semitones);
}

} // namespace stmlib

