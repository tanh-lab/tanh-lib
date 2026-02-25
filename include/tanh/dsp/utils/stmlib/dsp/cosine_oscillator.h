#pragma once

#include <tanh/dsp/utils/CosineOscillator.h>
#include <tanh/dsp/utils/stmlib/stmlib.h>

namespace stmlib {

using CosineOscillatorMode = thl::dsp::utils::CosineOscillatorMode;
inline constexpr CosineOscillatorMode COSINE_OSCILLATOR_APPROXIMATE = CosineOscillatorMode::Approximate;
inline constexpr CosineOscillatorMode COSINE_OSCILLATOR_EXACT = CosineOscillatorMode::Exact;

using thl::dsp::utils::CosineOscillator;

} // namespace stmlib

