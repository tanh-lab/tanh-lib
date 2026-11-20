#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/fx/ConstellationReverb.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr size_t k_block = 256;

using thl::core::BufferF;
using thl::core::BufferView;
using thl::dsp::fx::ConstellationReverbChannelMode;
using thl::dsp::fx::ConstellationReverbImpl;

/// The reverb on fixed parameters: a short decay, +12 shimmer.
class Reverb final : public ConstellationReverbImpl {
public:
    explicit Reverb(float shimmer, float predelay_ms = 0.0f) {
        m_values[PredelayMs] = predelay_ms;
        m_values[Decay] = 0.49f;
        m_values[Size] = 1.0f;
        m_values[Damping] = 0.5f;
        m_values[InputHpHz] = 20.0f;
        m_values[Shimmer] = shimmer;
        m_values[ShimmerSemitones] = 12.0f;
        m_values[FreqShiftHz] = 100.0f;
    }

private:
    float get_parameter_float(Parameter p, uint32_t /*modulation_offset*/) override {
        return m_values[static_cast<size_t>(p)];
    }
    bool get_parameter_bool(Parameter /*p*/, uint32_t /*modulation_offset*/) override {
        return false;
    }
    int get_parameter_int(Parameter /*p*/, uint32_t /*modulation_offset*/) override {
        return static_cast<int>(ConstellationReverbChannelMode::StereoToStereo);
    }

    std::array<float, NumParameters> m_values{};
};

/// One second of a 220 Hz sine through @p reverb: the left output.
std::vector<float> render(Reverb& reverb) {
    reverb.prepare(k_sample_rate, k_block, 2);
    std::vector<float> out;
    const auto total = static_cast<size_t>(k_sample_rate);
    for (size_t start = 0; start < total; start += k_block) {
        BufferF buffer(2, k_block, k_sample_rate);
        for (size_t i = 0; i < k_block; ++i) {
            const double t = static_cast<double>(start + i) / k_sample_rate;
            const auto x = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * t));
            buffer.get_write_pointer(0)[i] = x;
            buffer.get_write_pointer(1)[i] = x;
        }
        reverb.process(BufferView(buffer));
        const float* left = buffer.get_read_pointer(0);
        out.insert(out.end(), left, left + k_block);
    }
    return out;
}

/// The level of @p hz in the second half of @p x, in dB (Goertzel).
double level_db(const std::vector<float>& x, double hz) {
    const double w = 2.0 * std::numbers::pi * hz / k_sample_rate;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    for (size_t i = x.size() / 2; i < x.size(); ++i) {
        const double s = x[i] + (c * s1) - s2;
        s2 = s1;
        s1 = s;
    }
    const double power = (s1 * s1) + (s2 * s2) - (c * s1 * s2);
    return 10.0 * std::log10((power / static_cast<double>(x.size() / 2)) + 1e-20);
}

/// Energy of the second half of @p x between @p lo and @p hi Hz, in dB (2 Hz steps).
/// The shimmer's two-grain pitch shifter puts its output on lines spaced by its grain
/// rate (~23 Hz at 48 kHz) rather than exactly on the octave, so a single Goertzel bin
/// at 440 Hz can fall between two lines; the band catches all of them.
double band_db(const std::vector<float>& x, double lo, double hi) {
    double energy = 0.0;
    const auto steps = static_cast<int>((hi - lo) / 2.0);
    for (int i = 0; i <= steps; ++i) {
        energy += std::pow(10.0, level_db(x, lo + (2.0 * i)) / 10.0);
    }
    return 10.0 * std::log10(energy + 1e-20);
}

}  // namespace

TEST(ConstellationReverb, ShimmerSitsInTheFeedbackByDefault) {
    const Reverb reverb(0.5f);
    EXPECT_EQ(reverb.shimmer_position(), ConstellationReverbImpl::ShimmerPosition::Feedback);
}

// At the tank's input the shimmer is heard on the first pass: with a short decay its octave is
// far louder than in the feedback, where the decay takes it before it builds.
TEST(ConstellationReverb, ShimmerAtTheInputIsHeardWithAShortDecay) {
    Reverb feedback(0.5f);
    Reverb input(0.5f);
    input.set_shimmer_position(ConstellationReverbImpl::ShimmerPosition::Input);
    const double octave_feedback = band_db(render(feedback), 400.0, 520.0);
    const double octave_input = band_db(render(input), 400.0, 520.0);
    EXPECT_GT(octave_input, octave_feedback + 6.0)
        << "feedback " << octave_feedback << " dB, input " << octave_input << " dB";
}

namespace {

/// First output sample (left) whose magnitude exceeds a tiny threshold after a
/// unit impulse, or -1 if the reverb stays silent for one second.
long onset_of_impulse(float predelay_ms) {
    Reverb reverb(0.0f, predelay_ms);
    reverb.prepare(k_sample_rate, k_block, 2);
    const auto total = static_cast<size_t>(k_sample_rate);
    for (size_t start = 0; start < total; start += k_block) {
        BufferF buffer(2, k_block, k_sample_rate);
        if (start == 0) {
            buffer.get_write_pointer(0)[0] = 1.0f;
            buffer.get_write_pointer(1)[0] = 1.0f;
        }
        reverb.process(BufferView(buffer));
        const float* left = buffer.get_read_pointer(0);
        for (size_t i = 0; i < k_block; ++i) {
            if (std::abs(left[i]) > 1e-6f) { return static_cast<long>(start + i); }
        }
    }
    return -1;
}

}  // namespace

// A 0 ms predelay must not delay the input: 50 ms starts exactly 50 ms later.
// (The predelay line used to read its oldest slot at length 0, a full buffer late.)
TEST(ConstellationReverb, PredelayOfZeroAddsNoDelay) {
    const long at_zero = onset_of_impulse(0.0f);
    const long at_fifty = onset_of_impulse(50.0f);
    ASSERT_GE(at_zero, 0);
    ASSERT_GE(at_fifty, 0);
    EXPECT_EQ(at_fifty - at_zero, 2400);
}
