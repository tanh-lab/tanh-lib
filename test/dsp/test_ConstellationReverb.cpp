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
    explicit Reverb(float shimmer) {
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
    const double octave_feedback = level_db(render(feedback), 440.0);
    const double octave_input = level_db(render(input), 440.0);
    EXPECT_GT(octave_input, octave_feedback + 10.0)
        << "feedback " << octave_feedback << " dB, input " << octave_input << " dB";
}
