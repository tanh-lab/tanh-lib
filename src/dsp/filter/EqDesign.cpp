#include <tanh/dsp/filter/EqDesign.h>
#include <tanh/dsp/filter/SvfCoefficients.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <optional>

namespace thl::dsp::filter {

namespace {

constexpr double k_min_frequency = 10.0;
constexpr double k_max_frequency_ratio = 0.49;  // of the sample rate
constexpr double k_min_q = 0.025;
constexpr double k_max_q = 40.0;
constexpr double k_max_gain_db = 48.0;
constexpr EqBand k_defaults{};
// Above this Q the shelf fit misses a resonant shelf's bump by more than the bilinear shelf
// cramps (measured against the prototype, 44.1 to 96 kHz, up to +-48 dB): bilinear from here.
constexpr double k_matched_shelf_max_q = 2.0;

bool has_gain(EqBandType type) {
    return type == EqBandType::Peak || type == EqBandType::LowShelf ||
           type == EqBandType::HighShelf;
}

// The square root of the linear gain (RBJ's A).
double root_gain(const EqBand& band) {
    return std::pow(10.0, static_cast<double>(band.m_gain_db) / 40.0);
}

// --- Bilinear: Cytomic's SVF mixes (Simper 2013), the RBJ cookbook's curves ---------------------

SvfCoefficients design_bilinear(const EqBand& band, double sample_rate) TANH_NONBLOCKING_FUNCTION {
    const double g = std::tan(std::numbers::pi * band.m_frequency / sample_rate);
    const double q = band.m_q;
    const double k = 1.0 / q;
    const double a = root_gain(band);
    double cg = g;
    double ck = k;
    double m0 = 1.0;
    double m1 = 0.0;
    double m2 = 0.0;
    switch (band.m_type) {
        case EqBandType::Peak:
            ck = 1.0 / (q * a);
            m1 = ck * ((a * a) - 1.0);
            break;
        case EqBandType::Notch: m1 = -k; break;
        case EqBandType::LowShelf:
            cg = g / std::sqrt(a);
            m1 = k * (a - 1.0);
            m2 = (a * a) - 1.0;
            break;
        case EqBandType::HighShelf:
            cg = g * std::sqrt(a);
            m0 = a * a;
            m1 = k * (1.0 - a) * a;
            m2 = 1.0 - (a * a);
            break;
        case EqBandType::LowCut:
            m1 = -k;
            m2 = -1.0;
            break;
        case EqBandType::HighCut:
            m0 = 0.0;
            m2 = 1.0;
            break;
    }
    return {.m_g = cg, .m_k = ck, .m_m0 = m0, .m_m1 = m1, .m_m2 = m2};
}

// --- Matched: Vicanek, "Matched Second Order Digital Filters" (2016) ----------------------------

// Poles of s^2 + s w / q_p + w^2 by impulse invariance (Vicanek eq. 12), w in radians per sample.
struct Poles {
    double m_a1;
    double m_a2;
};

Poles impulse_invariant(double w, double pole_q) {
    const double q = 0.5 / pole_q;
    if (q <= 1.0) {
        const double e = std::exp(-q * w);
        return {.m_a1 = -2.0 * e * std::cos(std::sqrt(1.0 - (q * q)) * w), .m_a2 = e * e};
    }
    // Real poles: -2 e^(-q w) cosh(r w) as the sum of the two poles, so a large q w neither
    // underflows the exponential nor overflows the cosh (q - r = 1 / (q + r), no cancellation).
    const double r = std::sqrt((q * q) - 1.0);
    return {.m_a1 = -(std::exp(-w / (q + r)) + std::exp(-w * (q + r))),
            .m_a2 = std::exp(-2.0 * q * w)};
}

// |H(e^jw)|^2 = (B0 phi0 + B1 phi1 + B2 phi2) / (A0 phi0 + A1 phi1 + A2 phi2) (eq. 25 to 27).
struct Phi {
    double m_0;
    double m_1;
    double m_2;
};

Phi phi_at(double w) {
    const double s = std::sin(0.5 * w);
    const double p1 = s * s;
    const double p0 = 1.0 - p1;
    return {.m_0 = p0, .m_1 = p1, .m_2 = 4.0 * p0 * p1};
}

struct Squares {
    double m_0;
    double m_1;
    double m_2;

    [[nodiscard]] double at(const Phi& p) const {
        return (m_0 * p.m_0) + (m_1 * p.m_1) + (m_2 * p.m_2);
    }
};

Squares denominator_squares(const Poles& p) {
    const double dc = 1.0 + p.m_a1 + p.m_a2;
    const double ny = 1.0 - p.m_a1 + p.m_a2;
    return {.m_0 = dc * dc, .m_1 = ny * ny, .m_2 = -4.0 * p.m_a2};
}

// The minimum-phase numerator with squares B (eq. 29), or nothing where none exists.
std::optional<BiquadCoefficients> numerator_from(const Squares& b, const Poles& p) {
    if (!(b.m_0 >= 0.0) || !(b.m_1 >= 0.0)) { return std::nullopt; }
    const double r0 = std::sqrt(b.m_0);
    const double r1 = std::sqrt(b.m_1);
    const double w = 0.5 * (r0 + r1);
    const double d = (w * w) + b.m_2;
    if (!(d >= 0.0)) { return std::nullopt; }
    const double b0 = 0.5 * (w + std::sqrt(d));
    if (!(b0 > 0.0)) { return std::nullopt; }
    return BiquadCoefficients{.m_b0 = b0,
                              .m_b1 = 0.5 * (r0 - r1),
                              .m_b2 = -b.m_2 / (4.0 * b0),
                              .m_a1 = p.m_a1,
                              .m_a2 = p.m_a2};
}

double analog_power(const EqBand& band, double frequency) {
    return std::norm(analog_response(band, frequency));
}

// The magnitude of the prototype matched at DC, at the band's frequency and at Nyquist, with the
// prototype's poles (pole frequency @p pole_w, quality @p pole_q) by impulse invariance.
std::optional<BiquadCoefficients> fit_three_points(const EqBand& band,
                                                   double sample_rate,
                                                   double pole_w,
                                                   double pole_q) {
    const Poles p = impulse_invariant(pole_w, pole_q);
    const Squares a = denominator_squares(p);
    const Phi at_band = phi_at(2.0 * std::numbers::pi * band.m_frequency / sample_rate);
    Squares b{.m_0 = a.m_0 * analog_power(band, 0.0),
              .m_1 = a.m_1 * analog_power(band, 0.5 * sample_rate),
              .m_2 = 0.0};
    b.m_2 = ((a.at(at_band) * analog_power(band, band.m_frequency)) - (b.m_0 * at_band.m_0) -
             (b.m_1 * at_band.m_1)) /
            at_band.m_2;
    return numerator_from(b, p);
}

// The filter with numerator and denominator swapped: 1 / H.
std::optional<BiquadCoefficients> inverse(const std::optional<BiquadCoefficients>& c) {
    if (!c) { return std::nullopt; }
    return BiquadCoefficients{.m_b0 = 1.0 / c->m_b0,
                              .m_b1 = c->m_a1 / c->m_b0,
                              .m_b2 = c->m_a2 / c->m_b0,
                              .m_a1 = c->m_b1 / c->m_b0,
                              .m_a2 = c->m_b2 / c->m_b0};
}

std::optional<BiquadCoefficients> design_matched(const EqBand& band, double sample_rate) {
    const double w0 = 2.0 * std::numbers::pi * band.m_frequency / sample_rate;
    const double q = band.m_q;
    const Phi at_band = phi_at(w0);
    switch (band.m_type) {
        case EqBandType::Peak: {  // eq. 42 to 45
            const double a = root_gain(band);
            const Poles p = impulse_invariant(w0, q * a);
            const Squares s = denominator_squares(p);
            const double g2 = a * a * a * a;  // the linear gain, squared
            const double r1 = s.at(at_band) * g2;
            const double r2 = (-s.m_0 + s.m_1 + (4.0 * (at_band.m_0 - at_band.m_1) * s.m_2)) * g2;
            Squares b{.m_0 = s.m_0, .m_1 = 0.0, .m_2 = 0.0};
            b.m_2 = (r1 - (r2 * at_band.m_1) - b.m_0) / (4.0 * at_band.m_1 * at_band.m_1);
            b.m_1 = r2 + b.m_0 + (4.0 * (at_band.m_1 - at_band.m_0) * b.m_2);
            return numerator_from(b, p);
        }
        case EqBandType::HighCut: {  // eq. 30 to 33
            const Poles p = impulse_invariant(w0, q);
            const Squares s = denominator_squares(p);
            const double b0_square = s.m_0;
            const double b1_square =
                ((s.at(at_band) * q * q) - (b0_square * at_band.m_0)) / at_band.m_1;
            if (!(b1_square >= 0.0)) { return std::nullopt; }
            const double r0 = std::sqrt(b0_square);
            const double b0 = 0.5 * (r0 + std::sqrt(b1_square));
            return BiquadCoefficients{.m_b0 = b0,
                                      .m_b1 = r0 - b0,
                                      .m_b2 = 0.0,
                                      .m_a1 = p.m_a1,
                                      .m_a2 = p.m_a2};
        }
        case EqBandType::LowCut: {  // eq. 35, 36
            const Poles p = impulse_invariant(w0, q);
            const double b0 =
                std::sqrt(denominator_squares(p).at(at_band)) * q / (4.0 * at_band.m_1);
            return BiquadCoefficients{.m_b0 = b0,
                                      .m_b1 = -2.0 * b0,
                                      .m_b2 = b0,
                                      .m_a1 = p.m_a1,
                                      .m_a2 = p.m_a2};
        }
        case EqBandType::LowShelf:
        case EqBandType::HighShelf: {
            if (q > k_matched_shelf_max_q) { return std::nullopt; }
            // A shelf's poles sit at w0 / sqrt(A) (low) or w0 * sqrt(A) (high), its zeros at the
            // other: fit the shelf whose poles are the lower pair, so they stay below Nyquist, and
            // invert it when that is the opposite gain (a shelf at -G is 1 / the shelf at +G).
            const double a = root_gain(band);
            const bool low = band.m_type == EqBandType::LowShelf;
            const bool direct = low ? a >= 1.0 : a <= 1.0;
            EqBand fitted = band;
            if (!direct) { fitted.m_gain_db = -band.m_gain_db; }
            const double fitted_a = root_gain(fitted);
            const double pole_w = low ? w0 / std::sqrt(fitted_a) : w0 * std::sqrt(fitted_a);
            const auto c = fit_three_points(fitted, sample_rate, pole_w, q);
            return direct ? c : inverse(c);
        }
        case EqBandType::Notch: break;
    }
    return std::nullopt;
}

bool finite(const BiquadCoefficients& c) {
    return std::isfinite(c.m_b0) && std::isfinite(c.m_b1) && std::isfinite(c.m_b2) &&
           std::isfinite(c.m_a1) && std::isfinite(c.m_a2);
}

}  // namespace

EqBand sanitised(const EqBand& band, double sample_rate) TANH_NONBLOCKING_FUNCTION {
    EqBand b = band;
    if (static_cast<uint8_t>(b.m_type) > static_cast<uint8_t>(EqBandType::HighCut)) {
        b.m_type = k_defaults.m_type;
    }
    const auto fix = [](float value, float fallback, double lo, double hi) {
        const double v = std::isfinite(value) ? static_cast<double>(value) : fallback;
        return static_cast<float>(std::clamp(v, lo, hi));
    };
    b.m_frequency = fix(b.m_frequency,
                        k_defaults.m_frequency,
                        k_min_frequency,
                        std::max(k_min_frequency, k_max_frequency_ratio * sample_rate));
    b.m_gain_db = fix(b.m_gain_db, k_defaults.m_gain_db, -k_max_gain_db, k_max_gain_db);
    b.m_q = fix(b.m_q, k_defaults.m_q, k_min_q, k_max_q);
    return b;
}

bool is_flat(const EqBand& band) TANH_NONBLOCKING_FUNCTION {
    return !band.m_enabled || (has_gain(band.m_type) && band.m_gain_db == 0.0f);
}

SvfCoefficients design_band(const EqBand& band,
                            EqDesign design,
                            double sample_rate) TANH_NONBLOCKING_FUNCTION {
    const EqBand b = sanitised(band, sample_rate);
    if (is_flat(b)) { return SvfCoefficients::identity(); }
    if (design == EqDesign::Matched) {
        const auto c = design_matched(b, sample_rate);
        if (c && finite(*c) && c->is_stable()) { return SvfCoefficients::from_biquad(*c); }
    }
    return design_bilinear(b, sample_rate);
}

std::complex<double> analog_response(const EqBand& band, double frequency) {
    const EqBand b = sanitised(band, 1.0e12);  // no sample rate: no upper limit
    if (is_flat(b)) { return 1.0; }
    const std::complex<double> s(0.0, frequency / b.m_frequency);
    const double q = b.m_q;
    const double a = root_gain(b);
    const double ra = std::sqrt(a);
    const std::complex<double> s2 = s * s;
    switch (b.m_type) {
        case EqBandType::Peak: return (s2 + (s * a / q) + 1.0) / (s2 + (s / (a * q)) + 1.0);
        case EqBandType::Notch: return (s2 + 1.0) / (s2 + (s / q) + 1.0);
        case EqBandType::LowShelf:
            return a * (s2 + (s * ra / q) + a) / ((a * s2) + (s * ra / q) + 1.0);
        case EqBandType::HighShelf:
            return a * ((a * s2) + (s * ra / q) + 1.0) / (s2 + (s * ra / q) + a);
        case EqBandType::LowCut: return s2 / (s2 + (s / q) + 1.0);
        case EqBandType::HighCut: return 1.0 / (s2 + (s / q) + 1.0);
    }
    return 1.0;
}

}  // namespace thl::dsp::filter
