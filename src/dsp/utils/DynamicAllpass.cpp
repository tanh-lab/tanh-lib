#include <tanh/dsp/utils/DynamicAllpass.h>
#include <cstddef>

namespace thl::dsp::utils {

DynamicAllpass::DynamicAllpass() = default;
DynamicAllpass::~DynamicAllpass() = default;

void DynamicAllpass::prepare(size_t max_delay) {
    m_line.prepare(max_delay);
}

void DynamicAllpass::reset() {
    m_line.reset();
}

float DynamicAllpass::process(float sample, float delay, float coefficient) {
    const float r = m_line.read(delay);
    const float w = sample + coefficient * r;
    m_line.write(w);
    return -w * coefficient + r;
}

float DynamicAllpass::tap(float offset) const {
    return m_line.tap(offset);
}

float DynamicAllpass::tap(size_t offset) const {
    return m_line.tap(offset);
}

DynamicDelayLine& DynamicAllpass::line() {
    return m_line;
}

const DynamicDelayLine& DynamicAllpass::line() const {
    return m_line;
}

}  // namespace thl::dsp::utils
