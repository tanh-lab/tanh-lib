#pragma once

#include <tanh/dsp/utils/Svf.h>
#include <tanh/dsp/utils/stmlib/stmlib.h>

namespace stmlib {

using FilterMode = thl::dsp::utils::FilterMode;
inline constexpr FilterMode FILTER_MODE_LOW_PASS = FilterMode::LowPass;
inline constexpr FilterMode FILTER_MODE_BAND_PASS = FilterMode::BandPass;
inline constexpr FilterMode FILTER_MODE_BAND_PASS_NORMALIZED = FilterMode::BandPassNormalized;
inline constexpr FilterMode FILTER_MODE_HIGH_PASS = FilterMode::HighPass;

using FrequencyApproximation = thl::dsp::utils::FrequencyApproximation;
inline constexpr FrequencyApproximation FREQUENCY_EXACT = FrequencyApproximation::Exact;
inline constexpr FrequencyApproximation FREQUENCY_ACCURATE = FrequencyApproximation::Accurate;
inline constexpr FrequencyApproximation FREQUENCY_FAST = FrequencyApproximation::Fast;
inline constexpr FrequencyApproximation FREQUENCY_DIRTY = FrequencyApproximation::Dirty;

using thl::dsp::utils::DCBlocker;
using thl::dsp::utils::NaiveSvf;
using thl::dsp::utils::OnePole;
using thl::dsp::utils::Svf;

} // namespace stmlib

