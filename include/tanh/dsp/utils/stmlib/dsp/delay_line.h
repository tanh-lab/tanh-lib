#pragma once

#include <tanh/dsp/utils/DelayLine.h>
#include <tanh/dsp/utils/stmlib/stmlib.h>

namespace stmlib {
template<typename T, size_t max_delay>
using DelayLine = thl::dsp::utils::DelayLine<T, max_delay>;
} // namespace stmlib

