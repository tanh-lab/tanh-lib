#include <tanh/core/Numbers.h>
#include <tanh/dsp/utils/HannTable.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace thl::dsp::utils {

HannTable::HannTable() {
    for (size_t i = 0; i <= k_segments; ++i) {
        m_table[i] = exact(static_cast<float>(i) / static_cast<float>(k_segments));
    }
}

float HannTable::at(float phase) const {
    phase = std::clamp(phase, 0.0f, 0.9999f);
    float const x = phase * static_cast<float>(k_segments);
    auto const i = static_cast<size_t>(x);
    float const frac = x - static_cast<float>(i);
    return m_table[i] + (m_table[i + 1] - m_table[i]) * frac;
}

float HannTable::exact(float phase) {
    phase = std::clamp(phase, 0.0f, 0.9999f);
    return 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * phase));
}

const HannTable& HannTable::shared() {
    static const HannTable k_shared;
    return k_shared;
}

}  // namespace thl::dsp::utils
