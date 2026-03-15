#pragma once

#include <cstddef>

#include <tanh/dsp/utils/DelayLine.h>

namespace thl::dsp::utils {

template <typename T, size_t max_delay>
class Allpass {
public:
    Allpass() = default;

    void prepare() { m_line.prepare(); }

    T process(T sample, size_t delay, T coefficient) {
        const T read = m_line.read(delay);
        const T w = sample + coefficient * read;
        m_line.write(w);
        return static_cast<T>(-w * coefficient + read);
    }

    DelayLine<T, max_delay>& line() { return m_line; }
    const DelayLine<T, max_delay>& line() const { return m_line; }

private:
    DelayLine<T, max_delay> m_line;

    Allpass(const Allpass&) = delete;
    Allpass& operator=(const Allpass&) = delete;
};

}  // namespace thl::dsp::utils
