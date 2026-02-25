#pragma once

#include <cmath>
#include <cstddef>
#include <numbers>

namespace thl::dsp::utils {

enum class FilterMode {
    LowPass,
    BandPass,
    BandPassNormalized,
    HighPass,
};

enum class FrequencyApproximation {
    Exact,
    Accurate,
    Fast,
    Dirty,
};

namespace detail {
constexpr float kPiF = std::numbers::pi_v<float>;
constexpr float kPiPow2 = kPiF * kPiF;
constexpr float kPiPow3 = kPiPow2 * kPiF;
constexpr float kPiPow5 = kPiPow3 * kPiPow2;
constexpr float kPiPow7 = kPiPow5 * kPiPow2;
constexpr float kPiPow9 = kPiPow7 * kPiPow2;
constexpr float kPiPow11 = kPiPow9 * kPiPow2;
} // namespace detail

class DCBlocker {
public:
    void Init(float pole) {
        x_ = 0.0f;
        y_ = 0.0f;
        pole_ = pole;
    }

    void Process(float* in_out, size_t size) {
        float x = x_;
        float y = y_;
        const float pole = pole_;
        while (size--) {
            const float old_x = x;
            x = *in_out;
            *in_out++ = y = y * pole + x - old_x;
        }
        x_ = x;
        y_ = y;
    }

private:
    float pole_ = 0.0f;
    float x_ = 0.0f;
    float y_ = 0.0f;
};

class OnePole {
public:
    void Init() {
        set_f<FrequencyApproximation::Dirty>(0.01f);
        Reset();
    }

    void Reset() {
        state_ = 0.0f;
    }

    template <FrequencyApproximation approximation>
    static float tan(float f) {
        if constexpr (approximation == FrequencyApproximation::Exact) {
            f = f < 0.497f ? f : 0.497f;
            return std::tan(detail::kPiF * f);
        } else if constexpr (approximation == FrequencyApproximation::Dirty) {
            const float a = 3.736e-01f * detail::kPiPow3;
            return f * (detail::kPiF + a * f * f);
        } else if constexpr (approximation == FrequencyApproximation::Fast) {
            const float a = 3.260e-01f * detail::kPiPow3;
            const float b = 1.823e-01f * detail::kPiPow5;
            const float f2 = f * f;
            return f * (detail::kPiF + f2 * (a + b * f2));
        } else {
            const float a = 3.333314036e-01f * detail::kPiPow3;
            const float b = 1.333923995e-01f * detail::kPiPow5;
            const float c = 5.33740603e-02f * detail::kPiPow7;
            const float d = 2.900525e-03f * detail::kPiPow9;
            const float e = 9.5168091e-03f * detail::kPiPow11;
            const float f2 = f * f;
            return f * (detail::kPiF + f2 * (a + f2 * (b + f2 * (c + f2 * (d + f2 * e)))));
        }
    }

    template <FrequencyApproximation approximation>
    void set_f(float f) {
        g_ = tan<approximation>(f);
        gi_ = 1.0f / (1.0f + g_);
    }

    template <FilterMode mode>
    float Process(float in) {
        const float lp = (g_ * in + state_) * gi_;
        state_ = g_ * (in - lp) + lp;

        if constexpr (mode == FilterMode::LowPass) {
            return lp;
        } else if constexpr (mode == FilterMode::HighPass) {
            return in - lp;
        } else {
            return 0.0f;
        }
    }

private:
    float g_ = 0.0f;
    float gi_ = 1.0f;
    float state_ = 0.0f;
};

class Svf {
public:
    void Init() {
        set_f_q<FrequencyApproximation::Dirty>(0.01f, 100.0f);
        Reset();
    }

    void Reset() {
        state_1_ = 0.0f;
        state_2_ = 0.0f;
    }

    void set(const Svf& f) {
        g_ = f.g();
        r_ = f.r();
        h_ = f.h();
    }

    void set_g_r_h(float g, float r, float h) {
        g_ = g;
        r_ = r;
        h_ = h;
    }

    void set_g_r(float g, float r) {
        g_ = g;
        r_ = r;
        h_ = 1.0f / (1.0f + r_ * g_ + g_ * g_);
    }

    void set_g_q(float g, float resonance) {
        g_ = g;
        r_ = 1.0f / resonance;
        h_ = 1.0f / (1.0f + r_ * g_ + g_ * g_);
    }

    template <FrequencyApproximation approximation>
    void set_f_q(float f, float resonance) {
        g_ = OnePole::tan<approximation>(f);
        r_ = 1.0f / resonance;
        h_ = 1.0f / (1.0f + r_ * g_ + g_ * g_);
    }

    template <FilterMode mode>
    float Process(float in) {
        float hp = (in - r_ * state_1_ - g_ * state_1_ - state_2_) * h_;
        float bp = g_ * hp + state_1_;
        state_1_ = g_ * hp + bp;
        float lp = g_ * bp + state_2_;
        state_2_ = g_ * bp + lp;
        return select<mode>(hp, bp, lp);
    }

    template <FilterMode mode>
    void Process(const float* in, float* out, size_t size) {
        float state_1 = state_1_;
        float state_2 = state_2_;
        while (size--) {
            float hp = (*in - r_ * state_1 - g_ * state_1 - state_2) * h_;
            float bp = g_ * hp + state_1;
            state_1 = g_ * hp + bp;
            float lp = g_ * bp + state_2;
            state_2 = g_ * bp + lp;
            *out++ = select<mode>(hp, bp, lp);
            ++in;
        }
        state_1_ = state_1;
        state_2_ = state_2;
    }

    template <FilterMode mode>
    void Process(const float* in, float* out, size_t size, size_t stride) {
        float state_1 = state_1_;
        float state_2 = state_2_;
        while (size--) {
            float hp = (*in - r_ * state_1 - g_ * state_1 - state_2) * h_;
            float bp = g_ * hp + state_1;
            state_1 = g_ * hp + bp;
            float lp = g_ * bp + state_2;
            state_2 = g_ * bp + lp;
            *out = select<mode>(hp, bp, lp);
            out += stride;
            in += stride;
        }
        state_1_ = state_1;
        state_2_ = state_2;
    }

    void ProcessMultimode(const float* in, float* out, size_t size, float mode) {
        float state_1 = state_1_;
        float state_2 = state_2_;

        mode *= mode;
        const float hp_gain = mode < 0.5f ? mode * 2.0f : 2.0f - mode * 2.0f;
        const float lp_gain = mode < 0.5f ? 1.0f - mode * 2.0f : 0.0f;
        const float bp_gain = mode < 0.5f ? 0.0f : mode * 2.0f - 1.0f;

        while (size--) {
            float hp = (*in - r_ * state_1 - g_ * state_1 - state_2) * h_;
            float bp = g_ * hp + state_1;
            state_1 = g_ * hp + bp;
            float lp = g_ * bp + state_2;
            state_2 = g_ * bp + lp;
            *out++ = hp_gain * hp + bp_gain * bp + lp_gain * lp;
            ++in;
        }
        state_1_ = state_1;
        state_2_ = state_2;
    }

    template <FilterMode mode>
    void Process(const float* in, float* out_1, float* out_2, size_t size, float gain_1, float gain_2) {
        float state_1 = state_1_;
        float state_2 = state_2_;
        while (size--) {
            float hp = (*in - r_ * state_1 - g_ * state_1 - state_2) * h_;
            float bp = g_ * hp + state_1;
            state_1 = g_ * hp + bp;
            float lp = g_ * bp + state_2;
            state_2 = g_ * bp + lp;
            const float value = select<mode>(hp, bp, lp);
            *out_1++ += value * gain_1;
            *out_2++ += value * gain_2;
            ++in;
        }
        state_1_ = state_1;
        state_2_ = state_2;
    }

    float g() const { return g_; }
    float r() const { return r_; }
    float h() const { return h_; }

private:
    template <FilterMode mode>
    float select(float hp, float bp, float lp) const {
        if constexpr (mode == FilterMode::LowPass) {
            return lp;
        } else if constexpr (mode == FilterMode::BandPass) {
            return bp;
        } else if constexpr (mode == FilterMode::BandPassNormalized) {
            return bp * r_;
        } else {
            return hp;
        }
    }

    float g_ = 0.0f;
    float r_ = 0.0f;
    float h_ = 0.0f;
    float state_1_ = 0.0f;
    float state_2_ = 0.0f;
};

class NaiveSvf {
public:
    void Init() {
        set_f_q<FrequencyApproximation::Dirty>(0.01f, 100.0f);
        Reset();
    }

    void Reset() {
        lp_ = 0.0f;
        bp_ = 0.0f;
    }

    template <FrequencyApproximation approximation>
    void set_f_q(float f, float resonance) {
        f = f < 0.497f ? f : 0.497f;
        if constexpr (approximation == FrequencyApproximation::Exact) {
            f_ = 2.0f * std::sin(detail::kPiF * f);
        } else {
            f_ = 2.0f * detail::kPiF * f;
        }
        damp_ = 1.0f / resonance;
    }

    template <FilterMode mode>
    float Process(float in) {
        const float bp_normalized = bp_ * damp_;
        const float notch = in - bp_normalized;
        lp_ += f_ * bp_;
        const float hp = notch - lp_;
        bp_ += f_ * hp;
        return select<mode>(hp, bp_, bp_normalized, lp_);
    }

    float lp() const { return lp_; }
    float bp() const { return bp_; }

    template <FilterMode mode>
    void Process(const float* in, float* out, size_t size) {
        float lp = lp_;
        float bp = bp_;
        while (size--) {
            const float bp_normalized = bp * damp_;
            const float notch = *in++ - bp_normalized;
            lp += f_ * bp;
            const float hp = notch - lp;
            bp += f_ * hp;
            *out++ = select<mode>(hp, bp, bp_normalized, lp);
        }
        lp_ = lp;
        bp_ = bp;
    }

    void Split(const float* in, float* low, float* high, size_t size) {
        float lp = lp_;
        float bp = bp_;
        while (size--) {
            const float bp_normalized = bp * damp_;
            const float notch = *in++ - bp_normalized;
            lp += f_ * bp;
            const float hp = notch - lp;
            bp += f_ * hp;
            *low++ = lp;
            *high++ = hp;
        }
        lp_ = lp;
        bp_ = bp;
    }

    template <FilterMode mode>
    void Process(const float* in, float* out, size_t size, size_t decimate) {
        float lp = lp_;
        float bp = bp_;
        size_t n = decimate - 1;
        while (size--) {
            const float bp_normalized = bp * damp_;
            const float notch = *in++ - bp_normalized;
            lp += f_ * bp;
            const float hp = notch - lp;
            bp += f_ * hp;

            ++n;
            if (n == decimate) {
                *out++ = select<mode>(hp, bp, bp_normalized, lp);
                n = 0;
            }
        }
        lp_ = lp;
        bp_ = bp;
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

    float f_ = 0.0f;
    float damp_ = 0.0f;
    float lp_ = 0.0f;
    float bp_ = 0.0f;
};

} // namespace thl::dsp::utils

