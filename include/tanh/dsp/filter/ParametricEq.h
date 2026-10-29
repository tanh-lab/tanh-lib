#pragma once

#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/BaseProcessor.h>
#include <tanh/dsp/filter/EqDesign.h>
#include <tanh/dsp/filter/SvfCoefficients.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::dsp::filter {

/**
 * @brief A parametric equaliser of up to k_max_bands bands in series, for any number of channels.
 *
 * Each band is designed by design_band() (EqDesign::Matched by default, set_design()) and runs as
 * one trapezoidal state-variable filter per channel (SvfTick). Every change of a band (frequency,
 * gain, Q, type, on or off) and of the design ramps its coefficients over k_ramp_ms (SvfRamp), so
 * automation and a dragged band do not click; a disabled or flat band costs nothing once its
 * ramp ends.
 *
 * For drawing: response_curve() fills the curve the bands settle to (the same designs, so the
 * curve is what is heard), from settings the caller keeps, on any thread.
 *
 * Threads: prepare() allocates (audio stopped). set_band(), set_design(), process(), reset() and
 * band() belong to the audio thread and are real-time safe. The static functions are pure.
 * Denormals are left to the host's flush-to-zero.
 */
class TANH_API ParametricEq {
public:
    static constexpr size_t k_max_bands = 8;
    static constexpr double k_ramp_ms = 10.0;

    /// Allocate the state for @p num_channels channels and clear it. The bands keep their
    /// settings (all off at first), redesigned for @p sample_rate without a ramp.
    void prepare(double sample_rate, size_t num_channels);

    /// Set band @p index (< k_max_bands; others are ignored). A change ramps; an unchanged band
    /// costs a comparison, so a caller may set every band every block.
    void set_band(size_t index, const EqBand& band) TANH_NONBLOCKING_FUNCTION;
    /// The settings of band @p index, as set (audio thread).
    [[nodiscard]] const EqBand& band(size_t index) const { return m_bands[index].m_settings; }

    /// How every band is designed; a change ramps.
    void set_design(EqDesign design) TANH_NONBLOCKING_FUNCTION;
    [[nodiscard]] EqDesign design() const { return m_design; }

    /// Filter @p num_samples of the first @p num_channels channels in place; channels beyond
    /// the prepared count pass unchanged.
    void process(float* const* channels,
                 size_t num_channels,
                 size_t num_samples) TANH_NONBLOCKING_FUNCTION;

    /// Clear the filters' state and end every ramp at its target.
    void reset() TANH_NONBLOCKING_FUNCTION;

    /// The gain in dB at @p frequency of @p band settled.
    [[nodiscard]] static float magnitude_db(const EqBand& band,
                                            EqDesign design,
                                            double frequency,
                                            double sample_rate);
    /// The gain in dB at @p frequency of @p bands in series, settled.
    [[nodiscard]] static float response_db(std::span<const EqBand> bands,
                                           EqDesign design,
                                           double frequency,
                                           double sample_rate);
    /// The curve to plot: @p out_db[i] = response_db() at @p frequencies[i] (the shorter span
    /// decides). Designs each band once.
    static void response_curve(std::span<const EqBand> bands,
                               EqDesign design,
                               double sample_rate,
                               std::span<const float> frequencies,
                               std::span<float> out_db);
    /// @p out filled with frequencies spaced evenly on a log axis from @p lowest to @p highest
    /// (both included): the x axis of response_curve().
    static void log_frequencies(std::span<float> out, float lowest, float highest);

private:
    struct Band {
        EqBand m_settings{.m_enabled = false};
        SvfRamp m_ramp;
        bool m_flat = true;  // the target is the identity: skipped once the ramp ends
    };

    void retarget(Band& band, bool ramp) TANH_NONBLOCKING_FUNCTION;
    void process_band(size_t index, float* const* channels, size_t num_channels, size_t num_samples)
        TANH_NONBLOCKING_FUNCTION;

    double m_sample_rate = 48000.0;
    size_t m_num_channels = 0;
    EqDesign m_design = EqDesign::Matched;
    std::array<Band, k_max_bands> m_bands{};
    std::vector<SvfState> m_state;  // band-major: k_max_bands x m_num_channels
};

/**
 * @brief ParametricEq as a tanh processor: its parameters come from get_band_parameter_float()
 * and get_parameter_float(), so State and Modulation drive it like any BaseProcessor.
 *
 * A derived class gives every band's type (an EqBandType as a float, rounded), frequency (Hz),
 * gain (dB), Q and enabled (> 0.5), and the design (an EqDesign as a float, rounded). They are
 * read at the start of every process() call, at @p modulation_offset, so process_modulated()
 * applies them at its change points; every change ramps (ParametricEq).
 */
class TANH_API ParametricEqImpl : public BaseProcessor {
public:
    /// An equaliser of @p num_bands bands (1 to ParametricEq::k_max_bands).
    explicit ParametricEqImpl(size_t num_bands = ParametricEq::k_max_bands);

    void prepare(const double& sample_rate,
                 const size_t& samples_per_block,
                 const size_t& num_channels) override;
    void process(thl::core::BufferView buffer,
                 uint32_t modulation_offset = 0) TANH_NONBLOCKING_FUNCTION override;

    [[nodiscard]] size_t num_bands() const { return m_num_bands; }
    /// The equaliser itself (audio thread), e.g. for band() of what was read last.
    [[nodiscard]] const ParametricEq& equaliser() const { return m_eq; }

protected:
    enum Parameter {
        Design = 0,

        NumParameters = 1
    };

    enum BandParameter {
        Type = 0,
        Frequency = 1,
        Gain = 2,
        Q = 3,
        Enabled = 4,

        NumBandParameters = 5
    };

    virtual float get_parameter_float(Parameter parameter, uint32_t modulation_offset = 0) = 0;
    virtual float get_band_parameter_float(BandParameter parameter,
                                           size_t band,
                                           uint32_t modulation_offset = 0) = 0;

private:
    void read_parameters(uint32_t modulation_offset) TANH_NONBLOCKING_FUNCTION;

    ParametricEq m_eq;
    size_t m_num_bands;
};

}  // namespace thl::dsp::filter
