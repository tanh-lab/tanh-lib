#pragma once

#include <cstddef>

#include <tanh/dsp/filter/OnePole.h>

namespace thl::dsp::filter {

// TPT (Topology-Preserving Transform) state-variable filter.
//
// Two-pole/two-zero SVF derived by applying the bilinear transform to the
// analogue state-variable prototype and prewarping the cutoff so the digital
// response matches the analogue one at the target frequency. The trapezoidal
// integration scheme makes it unconditionally stable regardless of modulation
// rate.
//
// Simultaneously produces low-pass, band-pass, and high-pass outputs; the
// FilterMode template parameter selects which is returned.
//
// Coefficients:
//   g = tan(pi * f / f_s)   -- prewarped integrator gain (via OnePole::tan)
//   r = 1 / Q               -- damping
//   h = 1 / (1 + r*g + g*g) -- normalisation factor
class Svf {
public:
    void reset() {
        m_state_1 = 0.0f;
        m_state_2 = 0.0f;
    }

    void set(const Svf& f) {
        m_g = f.g();
        m_r = f.r();
        m_h = f.h();
    }

    void set_g_r_h(float g, float r, float h) {
        m_g = g;
        m_r = r;
        m_h = h;
    }

    void set_g_r(float g, float r) {
        m_g = g;
        m_r = r;
        m_h = 1.0f / (1.0f + m_r * m_g + m_g * m_g);
    }

    void set_g_q(float g, float resonance) {
        m_g = g;
        m_r = 1.0f / resonance;
        m_h = 1.0f / (1.0f + m_r * m_g + m_g * m_g);
    }

    template <Approximation approximation>
    void set_f_q(float f, float resonance) {
        m_g = OnePole::tan<approximation>(f);
        m_r = 1.0f / resonance;
        m_h = 1.0f / (1.0f + m_r * m_g + m_g * m_g);
    }

    template <FilterMode mode>
    float process(float in) {
        float hp = (in - m_r * m_state_1 - m_g * m_state_1 - m_state_2) * m_h;
        float bp = m_g * hp + m_state_1;
        m_state_1 = m_g * hp + bp;
        float lp = m_g * bp + m_state_2;
        m_state_2 = m_g * bp + lp;
        return select<mode>(hp, bp, lp);
    }

    template <FilterMode mode>
    void process(const float* in, float* out, size_t size) {
        float state_1 = m_state_1;
        float state_2 = m_state_2;
        while (size--) {
            float hp = (*in - m_r * state_1 - m_g * state_1 - state_2) * m_h;
            float bp = m_g * hp + state_1;
            state_1 = m_g * hp + bp;
            float lp = m_g * bp + state_2;
            state_2 = m_g * bp + lp;
            *out++ = select<mode>(hp, bp, lp);
            ++in;
        }
        m_state_1 = state_1;
        m_state_2 = state_2;
    }

    template <FilterMode mode>
    void process(const float* in, float* out, size_t size, size_t stride) {
        float state_1 = m_state_1;
        float state_2 = m_state_2;
        while (size--) {
            float hp = (*in - m_r * state_1 - m_g * state_1 - state_2) * m_h;
            float bp = m_g * hp + state_1;
            state_1 = m_g * hp + bp;
            float lp = m_g * bp + state_2;
            state_2 = m_g * bp + lp;
            *out = select<mode>(hp, bp, lp);
            out += stride;
            in += stride;
        }
        m_state_1 = state_1;
        m_state_2 = state_2;
    }

    void process_multimode(const float* in, float* out, size_t size, float mode) {
        float state_1 = m_state_1;
        float state_2 = m_state_2;

        mode *= mode;
        const float hp_gain = mode < 0.5f ? mode * 2.0f : 2.0f - mode * 2.0f;
        const float lp_gain = mode < 0.5f ? 1.0f - mode * 2.0f : 0.0f;
        const float bp_gain = mode < 0.5f ? 0.0f : mode * 2.0f - 1.0f;

        while (size--) {
            float hp = (*in - m_r * state_1 - m_g * state_1 - state_2) * m_h;
            float bp = m_g * hp + state_1;
            state_1 = m_g * hp + bp;
            float lp = m_g * bp + state_2;
            state_2 = m_g * bp + lp;
            *out++ = hp_gain * hp + bp_gain * bp + lp_gain * lp;
            ++in;
        }
        m_state_1 = state_1;
        m_state_2 = state_2;
    }

    template <FilterMode mode>
    void process(const float* in,
                 float* out_1,
                 float* out_2,
                 size_t size,
                 float gain_1,
                 float gain_2) {
        float state_1 = m_state_1;
        float state_2 = m_state_2;
        while (size--) {
            float hp = (*in - m_r * state_1 - m_g * state_1 - state_2) * m_h;
            float bp = m_g * hp + state_1;
            state_1 = m_g * hp + bp;
            float lp = m_g * bp + state_2;
            state_2 = m_g * bp + lp;
            const float value = select<mode>(hp, bp, lp);
            *out_1++ += value * gain_1;
            *out_2++ += value * gain_2;
            ++in;
        }
        m_state_1 = state_1;
        m_state_2 = state_2;
    }

    float g() const { return m_g; }
    float r() const { return m_r; }
    float h() const { return m_h; }

private:
    template <FilterMode mode>
    float select(float hp, float bp, float lp) const {
        if constexpr (mode == FilterMode::LowPass) {
            return lp;
        } else if constexpr (mode == FilterMode::BandPass) {
            return bp;
        } else if constexpr (mode == FilterMode::BandPassNormalized) {
            return bp * m_r;
        } else {
            return hp;
        }
    }

    float m_g = 0.0f;
    float m_r = 0.0f;
    float m_h = 0.0f;
    float m_state_1 = 0.0f;
    float m_state_2 = 0.0f;
};

// Naive (Chamberlin) state-variable filter.
//
// Uses the classic two-integrator topology with forward Euler integration.
// Simpler and cheaper than the TPT form above, but only conditionally stable.
// The cutoff must stay well below Nyquist/2 to avoid blowing up. Suitable
// for low-frequency filtering or quick-and-dirty spectral splitting where the
// frequency never approaches Nyquist.
class NaiveSvf {
public:
    void reset() {
        m_lp = 0.0f;
        m_bp = 0.0f;
    }

    template <Approximation approximation>
    void set_f_q(float f, float resonance) {
        f = f < 0.497f ? f : 0.497f;
        if constexpr (approximation == Approximation::Exact) {
            m_f = 2.0f * std::sin(detail::kPiF * f);
        } else {
            m_f = 2.0f * detail::kPiF * f;
        }
        m_damp = 1.0f / resonance;
    }

    template <FilterMode mode>
    float process(float in) {
        const float bp_normalized = m_bp * m_damp;
        const float notch = in - bp_normalized;
        m_lp += m_f * m_bp;
        const float hp = notch - m_lp;
        m_bp += m_f * hp;
        return select<mode>(hp, m_bp, bp_normalized, m_lp);
    }

    float lp() const { return m_lp; }
    float bp() const { return m_bp; }

    template <FilterMode mode>
    void process(const float* in, float* out, size_t size) {
        float lp = m_lp;
        float bp = m_bp;
        while (size--) {
            const float bp_normalized = bp * m_damp;
            const float notch = *in++ - bp_normalized;
            lp += m_f * bp;
            const float hp = notch - lp;
            bp += m_f * hp;
            *out++ = select<mode>(hp, bp, bp_normalized, lp);
        }
        m_lp = lp;
        m_bp = bp;
    }

    void split(const float* in, float* low, float* high, size_t size) {
        float lp = m_lp;
        float bp = m_bp;
        while (size--) {
            const float bp_normalized = bp * m_damp;
            const float notch = *in++ - bp_normalized;
            lp += m_f * bp;
            const float hp = notch - lp;
            bp += m_f * hp;
            *low++ = lp;
            *high++ = hp;
        }
        m_lp = lp;
        m_bp = bp;
    }

    template <FilterMode mode>
    void process(const float* in, float* out, size_t size, size_t decimate) {
        float lp = m_lp;
        float bp = m_bp;
        size_t n = decimate - 1;
        while (size--) {
            const float bp_normalized = bp * m_damp;
            const float notch = *in++ - bp_normalized;
            lp += m_f * bp;
            const float hp = notch - lp;
            bp += m_f * hp;

            ++n;
            if (n == decimate) {
                *out++ = select<mode>(hp, bp, bp_normalized, lp);
                n = 0;
            }
        }
        m_lp = lp;
        m_bp = bp;
    }

private:
    template <FilterMode mode>
    static float select(float hp, float bp, float bp_normalized, float lp) {
        if constexpr (mode == FilterMode::LowPass) {
            return lp;
        } else if constexpr (mode == FilterMode::BandPass) {
            return bp;
        } else if constexpr (mode == FilterMode::BandPassNormalized) {
            return bp_normalized;
        } else {
            return hp;
        }
    }

    float m_f = 0.0f;
    float m_damp = 0.0f;
    float m_lp = 0.0f;
    float m_bp = 0.0f;
};

}  // namespace thl::dsp::filter
