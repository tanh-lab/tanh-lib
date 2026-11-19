#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/utils/SmoothedValue.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::utils {

/**
 * Mixes num_inputs() buffer views into one, each input scaled by a gain g from its weight w.
 *
 * In Mode::Sum the gains are the weights: g = w. In Mode::Mix the weights are normalised to their
 * p-norm, g = w / (sum of w^p)^(1/p), so the gains always have a p-norm of 1: only the ratios of
 * the weights matter, and p sets what the mix keeps constant. k_amplitude suits correlated inputs,
 * k_power uncorrelated ones, and k_peak keeps the largest weight's gain at 1. All weights 0 give
 * silence.
 *
 * Weights, p and the mode glide over the ramp time given to prepare(). In Mode::Mix the gains are
 * normalised after the glide, so the mix keeps its level while it glides. A change of mode
 * crossfades from the gains of one mode to those of the other.
 *
 * @par Real-Time Safety
 *   Everything but prepare() is real-time safe: no allocation, no locks. prepare() allocates.
 *
 * @par Threads
 *   Not thread-safe: one thread at a time calls every member (e.g. the audio thread, with the
 *   settings handed to it through atomics).
 */
class TANH_API Mixer {
public:
    /// How the weights become gains.
    enum class Mode : uint8_t {
        Sum,  ///< the gains are the weights
        Mix,  ///< the weights normalised to their p-norm: only their ratios matter
    };

    /// p for correlated inputs: the gains sum to 1, the amplitude stays.
    static constexpr float k_amplitude = 1.0f;
    /// p between amplitude and power: -4.5 dB each for two equal weights, for partly correlated
    /// inputs.
    static constexpr float k_compromise = 4.0f / 3.0f;
    /// p for uncorrelated inputs: the squares of the gains sum to 1, the power stays.
    static constexpr float k_power = 2.0f;
    /// p = infinity: the largest weight's gain is 1, the others keep their ratio to it.
    static constexpr float k_peak = std::numeric_limits<float>::infinity();

    /// Room for @p num_inputs inputs. Inputs it already had keep their weights, new ones get
    /// weight 1; every glide ends at its target. A @p ramp_seconds shorter than one sample (0
    /// included) glides over one sample.
    void prepare(double sample_rate, size_t num_inputs, double ramp_seconds = 0.02);

    /// The number of inputs prepare() made room for.
    size_t num_inputs() const { return m_target_weights.size(); }

    /// Glides from the gains of the current mode to those of @p mode.
    void set_mode(Mode mode) TANH_NONBLOCKING_FUNCTION;
    /// The mode set last (the target of a glide).
    Mode get_mode() const { return m_mode; }

    /// The p of the norm in Mode::Mix, clamped to [1, infinity]; glides. NaN is ignored.
    void set_p(float p) TANH_NONBLOCKING_FUNCTION;
    /// The p set last (the target of a glide).
    float get_p() const { return m_p; }

    /// The target weight of input @p input, clamped to 0 or more; glides. A non-finite weight is
    /// ignored.
    void set_weight(size_t input, float weight) TANH_NONBLOCKING_FUNCTION;
    /// The target weights of inputs 0 to weights.size() - 1, as set_weight(); the others keep
    /// theirs.
    void set_weights(std::span<const float> weights) TANH_NONBLOCKING_FUNCTION;
    /// The target weight of input @p input (< num_inputs()).
    float get_weight(size_t input) const;

    /// Ends every glide at its target.
    void reset() TANH_NONBLOCKING_FUNCTION;

    /// The mix is input @p input unchanged: it passes at gain 1, the others at 0, and nothing
    /// glides that could change that (a glide of p, or of the mode where the gain is 1 in both,
    /// changes nothing). A caller may then skip process(); a glide it skips resumes where it was
    /// when process() runs again.
    bool passes_only(size_t input) const TANH_NONBLOCKING_FUNCTION;

    /// Writes the mix of @p inputs (one per input, each with the channels and samples of
    /// @p output) into @p output. @p output may be one of @p inputs, but must not overlap any
    /// other way.
    void process(std::span<const thl::core::ConstBufferView> inputs,
                 thl::core::BufferView output) TANH_NONBLOCKING_FUNCTION;

private:
    bool is_gliding() const TANH_NONBLOCKING_FUNCTION;
    /// Channel @p channel of the mix at the current gains, into @p output.
    void mix_channel(std::span<const thl::core::ConstBufferView> inputs,
                     float* output,
                     size_t channel,
                     size_t num_samples) const TANH_NONBLOCKING_FUNCTION;
    /// Advances every glide by one sample and writes the gains there into m_gains.
    void update_gains() TANH_NONBLOCKING_FUNCTION;

    std::vector<SmoothedValue> m_weights;
    std::vector<float> m_target_weights;
    std::vector<float> m_gains;
    SmoothedValue m_inverse_p;   // 1/p: finite for p = infinity, so it can glide
    SmoothedValue m_mix_amount;  // 0: Mode::Sum, 1: Mode::Mix
    Mode m_mode = Mode::Sum;
    float m_p = k_amplitude;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::utils
