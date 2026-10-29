#include <gtest/gtest.h>
#include <tanh/dsp/filter/EqDesign.h>
#include <tanh/dsp/filter/SvfCoefficients.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

using thl::dsp::filter::analog_response;
using thl::dsp::filter::BiquadCoefficients;
using thl::dsp::filter::design_band;
using thl::dsp::filter::EqBand;
using thl::dsp::filter::EqBandType;
using thl::dsp::filter::EqDesign;
using thl::dsp::filter::SvfCoefficients;

namespace {

constexpr double k_fs = 48000.0;
constexpr std::array k_types{EqBandType::Peak,
                             EqBandType::Notch,
                             EqBandType::LowShelf,
                             EqBandType::HighShelf,
                             EqBandType::LowCut,
                             EqBandType::HighCut};

double db(std::complex<double> h) {
    return 20.0 * std::log10(std::max(std::abs(h), 1e-12));
}

double analog_db(const EqBand& band, double f) {
    return db(analog_response(band, f));
}

// The largest gap between a design and the prototype from 20 Hz to 95 % of Nyquist, where the
// prototype is above -40 dB (deep in a cut's stop band, dB differences say nothing).
double max_error_db(const EqBand& band, EqDesign design, double fs = k_fs) {
    const SvfCoefficients c = design_band(band, design, fs);
    double worst = 0.0;
    for (double f = 20.0; f < 0.95 * fs / 2.0; f *= 1.01) {
        const double a = analog_db(band, f);
        if (a < -40.0) { continue; }
        worst = std::max(worst, std::abs(c.magnitude_db(f, fs) - a));
    }
    return worst;
}

// H(z) evaluated directly, independent of SvfCoefficients::response().
std::complex<double> biquad_response(const BiquadCoefficients& c, double f) {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * f / k_fs);
    return (c.m_b0 + (c.m_b1 * z1) + (c.m_b2 * z1 * z1)) /
           (1.0 + (c.m_a1 * z1) + (c.m_a2 * z1 * z1));
}

EqBand band_of(EqBandType type, float f, float gain_db = 9.0f, float q = 0.70710678f) {
    return {.m_type = type, .m_frequency = f, .m_gain_db = gain_db, .m_q = q};
}

}  // namespace

// --- SvfCoefficients ------------------------------------------------------------------------

TEST(SvfCoefficients, BiquadRoundTripsAndKeepsTheResponse) {
    for (const auto type : k_types) {
        for (const float f : {60.0f, 1000.0f, 9000.0f, 19000.0f}) {
            for (const EqDesign design : {EqDesign::Bilinear, EqDesign::Matched}) {
                const SvfCoefficients svf =
                    design_band(band_of(type, f, -7.0f, 2.0f), design, k_fs);
                const BiquadCoefficients biquad = svf.to_biquad();
                ASSERT_TRUE(biquad.is_stable());
                const SvfCoefficients back = SvfCoefficients::from_biquad(biquad);
                for (const double probe : {30.0, 700.0, 5000.0, 15000.0, 22000.0}) {
                    const double expected = db(biquad_response(biquad, probe));
                    EXPECT_NEAR(svf.magnitude_db(probe, k_fs), expected, 1e-3)
                        << static_cast<int>(type) << " " << f << " at " << probe;
                    EXPECT_NEAR(back.magnitude_db(probe, k_fs), expected, 1e-3)
                        << static_cast<int>(type) << " " << f << " at " << probe;
                }
            }
        }
    }
}

TEST(SvfCoefficients, IdentityPassesEverything) {
    const SvfCoefficients c = SvfCoefficients::identity();
    EXPECT_TRUE(c.is_identity());
    EXPECT_EQ(c.magnitude_db(1234.0, k_fs), 0.0);
    EXPECT_EQ(c.response(1234.0, k_fs), std::complex<double>(1.0));
    const BiquadCoefficients b = c.to_biquad();
    EXPECT_TRUE(b.is_stable());
    EXPECT_EQ(b.m_b0, 1.0);
    EXPECT_EQ(b.m_b1, 0.0);
    EXPECT_EQ(b.m_a1, 0.0);
}

// --- the prototypes and the bilinear designs --------------------------------------------------

TEST(EqDesign, BilinearFollowsThePrototypeWellBelowNyquist) {
    // At 100 Hz and 48 kHz the warping is negligible below 1 kHz: both describe the same filter.
    for (const auto type : k_types) {
        for (const float q : {0.5f, 0.70710678f, 2.0f}) {
            const EqBand band = band_of(type, 100.0f, 9.0f, q);
            const SvfCoefficients c = design_band(band, EqDesign::Bilinear, k_fs);
            for (double f = 10.0; f < 1000.0; f *= 1.1) {
                // The notch's zero and a cut's stop band: dB differences say nothing there.
                if (analog_db(band, f) < -30.0) { continue; }
                EXPECT_NEAR(c.magnitude_db(f, k_fs), analog_db(band, f), 0.02)
                    << static_cast<int>(type) << " q " << q << " at " << f;
            }
        }
    }
}

TEST(EqDesign, FlatBandsAreTheIdentity) {
    for (const EqDesign design : {EqDesign::Bilinear, EqDesign::Matched}) {
        EXPECT_TRUE(
            design_band(band_of(EqBandType::Peak, 1000.0f, 0.0f), design, k_fs).is_identity());
        EXPECT_TRUE(
            design_band(band_of(EqBandType::HighShelf, 5000.0f, 0.0f), design, k_fs).is_identity());
        EqBand off = band_of(EqBandType::LowCut, 100.0f);
        off.m_enabled = false;
        EXPECT_TRUE(design_band(off, design, k_fs).is_identity());
        EXPECT_FALSE(design_band(band_of(EqBandType::LowCut, 100.0f), design, k_fs).is_identity());
    }
}

// --- the matched designs --------------------------------------------------------------------

TEST(EqDesign, MatchedFollowsThePrototypeUpToNyquist) {
    // Where the bilinear curve cramps (bands at 10 to 18 kHz at 48 kHz), the matched curve stays
    // within a fraction of it.
    for (const auto type : {EqBandType::Peak,
                            EqBandType::LowShelf,
                            EqBandType::HighShelf,
                            EqBandType::LowCut,
                            EqBandType::HighCut}) {
        for (const float f : {10000.0f, 15000.0f, 18000.0f}) {
            for (const float gain : {-12.0f, 12.0f}) {
                const EqBand band = band_of(type, f, gain, 0.70710678f);
                const double matched = max_error_db(band, EqDesign::Matched);
                const double bilinear = max_error_db(band, EqDesign::Bilinear);
                EXPECT_LT(matched, 2.5) << static_cast<int>(type) << " " << f << " " << gain;
                EXPECT_LT(matched, 0.5 * bilinear)
                    << static_cast<int>(type) << " " << f << " " << gain;
            }
        }
    }
}

TEST(EqDesign, MatchedPeakIsExactAtItsFrequencyAndAtDc) {
    for (const float f : {200.0f, 3000.0f, 12000.0f, 19000.0f}) {
        for (const float gain : {-15.0f, 6.0f, 15.0f}) {
            for (const float q : {0.5f, 2.0f, 8.0f}) {
                const SvfCoefficients c =
                    design_band(band_of(EqBandType::Peak, f, gain, q), EqDesign::Matched, k_fs);
                EXPECT_NEAR(c.magnitude_db(f, k_fs), gain, 0.01) << f << " " << gain << " " << q;
                EXPECT_NEAR(c.magnitude_db(1.0, k_fs), 0.0, 0.01) << f << " " << gain << " " << q;
            }
        }
    }
}

TEST(EqDesign, MatchedShelfIsExactAtDcItsFrequencyAndNyquist) {
    for (const auto type : {EqBandType::LowShelf, EqBandType::HighShelf}) {
        for (const float f : {300.0f, 4000.0f, 15000.0f}) {
            for (const float gain : {-12.0f, 9.0f}) {
                const EqBand band = band_of(type, f, gain);
                const SvfCoefficients c = design_band(band, EqDesign::Matched, k_fs);
                EXPECT_NEAR(c.magnitude_db(0.0, k_fs), analog_db(band, 0.0), 0.01);
                EXPECT_NEAR(c.magnitude_db(f, k_fs), analog_db(band, f), 0.01);
                EXPECT_NEAR(c.magnitude_db(0.4999 * k_fs, k_fs), analog_db(band, 0.5 * k_fs), 0.05);
            }
        }
    }
}

TEST(EqDesign, MatchedCutsAreExactAtTheirFrequency) {
    for (const auto type : {EqBandType::LowCut, EqBandType::HighCut}) {
        for (const float f : {50.0f, 2000.0f, 16000.0f}) {
            for (const float q : {0.5f, 0.70710678f, 4.0f}) {
                const EqBand band = band_of(type, f, 0.0f, q);
                const SvfCoefficients c = design_band(band, EqDesign::Matched, k_fs);
                EXPECT_NEAR(c.magnitude_db(f, k_fs), analog_db(band, f), 0.01) << f << " " << q;
            }
        }
    }
}

TEST(EqDesign, MatchedShelvesAreNoWorseThanBilinearUpToQ2) {
    // Checked against an independent Python reference (RBJ cookbook, scipy) on a wider grid.
    for (const double fs : {44100.0, 48000.0, 96000.0}) {
        for (const auto type : {EqBandType::LowShelf, EqBandType::HighShelf}) {
            for (float f = 100.0f; f < 0.46f * static_cast<float>(fs); f *= 1.6f) {
                for (const float gain : {-48.0f, -24.0f, -6.0f, -1.0f, 1.0f, 6.0f, 24.0f, 48.0f}) {
                    for (const float q : {0.3f, 0.70710678f, 1.2f, 2.0f}) {
                        const EqBand band = band_of(type, f, gain, q);
                        EXPECT_LE(max_error_db(band, EqDesign::Matched, fs),
                                  max_error_db(band, EqDesign::Bilinear, fs) + 0.2)
                            << fs << " " << static_cast<int>(type) << " " << f << " " << gain << " "
                            << q;
                    }
                }
            }
        }
    }
}

TEST(EqDesign, ResonantShelvesAreBilinear) {
    for (const auto type : {EqBandType::LowShelf, EqBandType::HighShelf}) {
        for (const float q : {2.01f, 3.0f, 12.0f, 40.0f}) {
            const EqBand band = band_of(type, 5000.0f, 12.0f, q);
            const SvfCoefficients m = design_band(band, EqDesign::Matched, k_fs);
            const SvfCoefficients b = design_band(band, EqDesign::Bilinear, k_fs);
            EXPECT_EQ(m.m_g, b.m_g) << q;
            EXPECT_EQ(m.m_k, b.m_k) << q;
            EXPECT_EQ(m.m_m1, b.m_m1) << q;
        }
    }
}

TEST(EqDesign, MatchedHoldsAtExtremeLowQ) {
    // Real poles with q w beyond 700: exp(-q w) and cosh(r w) alone would underflow and overflow.
    for (const float f : {17000.0f, 18000.0f, 20000.0f}) {
        const EqBand band = band_of(EqBandType::Peak, f, -48.0f, 0.025f);
        EXPECT_LT(max_error_db(band, EqDesign::Matched), max_error_db(band, EqDesign::Bilinear))
            << f;
        EXPECT_NE(design_band(band, EqDesign::Matched, k_fs).m_g,
                  design_band(band, EqDesign::Bilinear, k_fs).m_g)
            << f;  // the matched design, not the fallback
    }
}

TEST(EqDesign, MatchedFollowsThePrototypeAtOtherSampleRates) {
    for (const double fs : {44100.0, 96000.0, 192000.0}) {
        for (const auto type : {EqBandType::Peak, EqBandType::LowCut, EqBandType::HighCut}) {
            const EqBand band = band_of(type, static_cast<float>(0.35 * fs), 9.0f, 1.0f);
            EXPECT_LT(max_error_db(band, EqDesign::Matched, fs),
                      0.5 * max_error_db(band, EqDesign::Bilinear, fs))
                << fs << " " << static_cast<int>(type);
        }
    }
}

TEST(EqDesign, NotchStaysBilinear) {
    const EqBand band = band_of(EqBandType::Notch, 15000.0f, 0.0f, 2.0f);
    const SvfCoefficients m = design_band(band, EqDesign::Matched, k_fs);
    EXPECT_LT(m.magnitude_db(15000.0, k_fs), -100.0);  // the zero exactly at its frequency
}

TEST(EqDesign, EveryDesignIsStableAcrossTheRange) {
    for (const auto type : k_types) {
        for (float f = 10.0f; f < 0.5f * static_cast<float>(k_fs); f *= 1.5f) {
            for (const float gain : {-48.0f, -12.0f, -1.0f, 1.0f, 12.0f, 48.0f}) {
                for (const float q : {0.025f, 0.3f, 1.0f, 5.0f, 40.0f}) {
                    for (const EqDesign design : {EqDesign::Bilinear, EqDesign::Matched}) {
                        const SvfCoefficients c =
                            design_band(band_of(type, f, gain, q), design, k_fs);
                        ASSERT_TRUE(c.m_g > 0.0f && c.m_k > 0.0f && std::isfinite(c.m_m0) &&
                                    std::isfinite(c.m_m1) && std::isfinite(c.m_m2))
                            << static_cast<int>(type) << " " << f << " " << gain << " " << q;
                    }
                }
            }
        }
    }
}

TEST(EqDesign, SanitisedReplacesWhatIsOutOfRange) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const EqBand b = thl::dsp::filter::sanitised({.m_type = static_cast<EqBandType>(99),
                                                  .m_frequency = nan,
                                                  .m_gain_db = 100.0f,
                                                  .m_q = 0.0f},
                                                 k_fs);
    EXPECT_EQ(b.m_type, EqBandType::Peak);
    EXPECT_FLOAT_EQ(b.m_frequency, 1000.0f);
    EXPECT_FLOAT_EQ(b.m_gain_db, 48.0f);
    EXPECT_FLOAT_EQ(b.m_q, 0.025f);
    EXPECT_FLOAT_EQ(thl::dsp::filter::sanitised({.m_frequency = 30000.0f}, k_fs).m_frequency,
                    0.49f * static_cast<float>(k_fs));
}
