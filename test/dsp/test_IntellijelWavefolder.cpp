#include <gtest/gtest.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/BufferView.h>
#include <tanh/dsp/fx/IntellijelWavefolder.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <random>
#include <vector>

namespace {

constexpr double k_sample_rate = 48000.0;
constexpr size_t k_block_size = 64;

using thl::core::BufferF;
using thl::core::BufferView;

/// Wavefolder with plain settable parameters.
class TestWavefolder : public thl::dsp::fx::IntellijelWavefolderImpl {
public:
    float m_drive = 1.0f;
    float m_folds = 0.0f;
    float m_symmetry = 0.0f;
    float m_tone = 1.0f;

protected:
    float get_parameter_float(Parameter p, uint32_t /*modulation_offset*/) override {
        switch (p) {
            case Drive: return m_drive;
            case Folds: return m_folds;
            case Symmetry: return m_symmetry;
            case Tone: return m_tone;
            default: return 0.0f;
        }
    }
};

/// Runs `input` (mono) through `wf` block by block. `on_block` is called with
/// the block index before each block is processed.
std::vector<float> run(TestWavefolder& wf,
                       const std::vector<float>& input,
                       const std::function<void(size_t)>& on_block = {}) {
    std::vector<float> output(input.size());
    BufferF block(1, k_block_size, k_sample_rate);
    for (size_t start = 0, b = 0; start < input.size(); start += k_block_size, ++b) {
        const size_t n = std::min(k_block_size, input.size() - start);
        if (on_block) { on_block(b); }
        float* data = block.get_write_pointer(0);
        std::copy_n(input.data() + start, n, data);
        wf.process(BufferView(block).sub_block(0, n));
        std::copy_n(data, n, output.data() + start);
    }
    return output;
}

std::vector<float> sine(float amplitude, float freq_hz, size_t num_samples) {
    std::vector<float> out(num_samples);
    for (size_t i = 0; i < num_samples; ++i) {
        out[i] = amplitude * std::sin(2.0f * std::numbers::pi_v<float> * freq_hz *
                                      static_cast<float>(i) / static_cast<float>(k_sample_rate));
    }
    return out;
}

/// Overlapping Hann-windowed sine bursts at about -20 dBFS — the kind of signal
/// a granular voice feeds the folder.
std::vector<float> grain_signal(size_t num_samples) {
    std::vector<float> out(num_samples, 0.0f);
    constexpr size_t k_grain_length = 4801;  // ~100 ms
    constexpr size_t k_grain_hop = 1597;     // three grains overlap, off the block grid
    constexpr std::array<float, 4> k_freqs = {220.0f, 330.0f, 277.0f, 415.0f};
    size_t g = 0;
    for (size_t start = 0; start + k_grain_length <= num_samples; start += k_grain_hop, ++g) {
        const float f = k_freqs[g % k_freqs.size()];
        for (size_t i = 0; i < k_grain_length; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(k_grain_length);
            const float window = 0.5f - 0.5f * std::cos(2.0f * std::numbers::pi_v<float> * t);
            out[start + i] += 0.05f * window *
                              std::sin(2.0f * std::numbers::pi_v<float> * f *
                                       static_cast<float>(i) / static_cast<float>(k_sample_rate));
        }
    }
    return out;
}

/// Magnitude of the `freq_hz` component of `x` (Goertzel over whole cycles).
float harmonic_magnitude(const std::vector<float>& x, size_t start, size_t length, float freq_hz) {
    const double w = 2.0 * std::numbers::pi * freq_hz / k_sample_rate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    for (size_t i = start; i < start + length; ++i) {
        const double s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return static_cast<float>(2.0 * std::sqrt(std::max(power, 0.0)) / static_cast<double>(length));
}

/// Sum of harmonics 2..10 relative to the fundamental.
float thd(const std::vector<float>& x, size_t start, size_t length, float f0) {
    const float h1 = harmonic_magnitude(x, start, length, f0);
    float sum = 0.0f;
    for (int h = 2; h <= 10; ++h) {
        const float m = harmonic_magnitude(x, start, length, f0 * static_cast<float>(h));
        sum += m * m;
    }
    return std::sqrt(sum) / std::max(h1, 1e-12f);
}

bool all_finite(const std::vector<float>& x) {
    return std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
}

float max_step(const std::vector<float>& x) {
    float m = 0.0f;
    for (size_t i = 1; i < x.size(); ++i) { m = std::max(m, std::fabs(x[i] - x[i - 1])); }
    return m;
}

float peak(const std::vector<float>& x) {
    float m = 0.0f;
    for (const float v : x) { m = std::max(m, std::fabs(v)); }
    return m;
}

/// Energy of the third difference (a crude > 10 kHz high-pass) in the samples
/// right after each block boundary, relative to its mean over the whole signal.
/// Parameter steps that click land on block boundaries and push this above 1.
float boundary_hf_ratio(const std::vector<float>& x) {
    constexpr size_t k_window = 8;
    double boundary = 0.0;
    size_t boundary_count = 0;
    double total = 0.0;
    size_t total_count = 0;
    for (size_t i = 3; i < x.size(); ++i) {
        const double d = x[i] - 3.0 * x[i - 1] + 3.0 * x[i - 2] - x[i - 3];
        total += d * d;
        ++total_count;
        if (i % k_block_size < k_window) {
            boundary += d * d;
            ++boundary_count;
        }
    }
    const double mean_total = total / static_cast<double>(total_count);
    const double mean_boundary = boundary / static_cast<double>(boundary_count);
    return static_cast<float>(mean_boundary / std::max(mean_total, 1e-30));
}

}  // namespace

// ── Click safety ────────────────────────────────────────────────────────────

TEST(IntellijelWavefolder, RandomisingEveryBlockStaysSmoothAndFinite) {
    TestWavefolder wf;
    wf.prepare(k_sample_rate, k_block_size, 1);

    const auto input = grain_signal(static_cast<size_t>(k_sample_rate) * 4);
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const auto output = run(wf, input, [&](size_t) {
        wf.m_drive = 0.1f + 19.9f * unit(rng);
        wf.m_folds = 10.0f * unit(rng);
        wf.m_symmetry = -1.0f + 2.0f * unit(rng);
        wf.m_tone = unit(rng);
    });

    EXPECT_TRUE(all_finite(output));
    // Heavy folding is allowed to swing fast, but never by more than the
    // output range of a full-scale fold around the input envelope.
    const float in_peak = peak(input);
    EXPECT_LT(max_step(output), 4.0f * in_peak);
    EXPECT_LT(peak(output), 3.0f * in_peak);
    // No energy concentration where the parameters jump.
    EXPECT_LT(boundary_hf_ratio(output), 1.3f);
}

TEST(IntellijelWavefolder, RandomisingOneParameterAtLowDriveDoesNotCrackle) {
    // Gentle folding, so a parameter ramp that kinks the sine's phase at each
    // retarget stands out against the signal's own high-frequency content.
    enum Which { Drive, Folds, Symmetry, Tone };
    for (const Which which : {Drive, Folds, Symmetry, Tone}) {
        TestWavefolder wf;
        wf.m_drive = 1.0f;
        wf.m_folds = 1.0f;
        wf.prepare(k_sample_rate, k_block_size, 1);

        std::mt19937 rng(99);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const auto output =
            run(wf, grain_signal(static_cast<size_t>(k_sample_rate) * 2), [&](size_t) {
                switch (which) {
                    case Drive: wf.m_drive = 0.1f + 19.9f * unit(rng); break;
                    case Folds: wf.m_folds = 10.0f * unit(rng); break;
                    case Symmetry: wf.m_symmetry = -1.0f + 2.0f * unit(rng); break;
                    case Tone: wf.m_tone = unit(rng); break;
                }
            });

        EXPECT_TRUE(all_finite(output));
        EXPECT_LT(boundary_hf_ratio(output), 1.3f) << "parameter " << which;
    }
}

TEST(IntellijelWavefolder, SilenceStaysSilentWithOffsetSymmetry) {
    for (const float symmetry : {-1.0f, 1.0f}) {
        TestWavefolder wf;
        wf.m_drive = 20.0f;
        wf.m_folds = 10.0f;
        wf.m_symmetry = symmetry;
        wf.m_tone = 0.0f;
        wf.prepare(k_sample_rate, k_block_size, 1);

        const auto output = run(wf, std::vector<float>(48000, 0.0f));
        EXPECT_LT(peak(output), 1e-6f) << "symmetry " << symmetry;
    }
}

TEST(IntellijelWavefolder, GrainOnsetFromSilenceDoesNotSpike) {
    for (const float drive : {0.5f, 3.0f, 20.0f}) {
        TestWavefolder wf;
        wf.m_drive = drive;
        wf.m_folds = 2.0f;
        wf.m_symmetry = 0.3f;
        wf.prepare(k_sample_rate, k_block_size, 1);

        // 20 ms of silence, then a burst with a 1 ms linear fade-in.
        std::vector<float> input(9600, 0.0f);
        const auto burst = sine(0.2f, 440.0f, 8640);
        for (size_t i = 0; i < burst.size(); ++i) {
            const float fade = std::min(1.0f, static_cast<float>(i) / 48.0f);
            input[960 + i] = burst[i] * fade;
        }
        const auto output = run(wf, input);

        EXPECT_TRUE(all_finite(output));
        EXPECT_LT(peak(output), 2.5f * 0.2f) << "drive " << drive;
        const std::vector<float> onset(output.begin() + 960, output.begin() + 960 + 480);
        const std::vector<float> body(output.begin() + 4800, output.end());
        EXPECT_LT(peak(onset), 1.25f * std::max(peak(body), 0.05f)) << "drive " << drive;
    }
}

// ── Level independence ──────────────────────────────────────────────────────

namespace {

/// Third-to-first harmonic ratio of a 1 kHz sine at `level_db` dBFS after the
/// folder has settled.
float h3_ratio(float drive, float level_db) {
    TestWavefolder wf;
    wf.m_drive = drive;
    wf.prepare(k_sample_rate, k_block_size, 1);
    const float amplitude = std::pow(10.0f, level_db / 20.0f);
    const auto output = run(wf, sine(amplitude, 1000.0f, 48000));
    return harmonic_magnitude(output, 24000, 24000, 3000.0f) /
           harmonic_magnitude(output, 24000, 24000, 1000.0f);
}

float thd_at(float drive, float level_db) {
    TestWavefolder wf;
    wf.m_drive = drive;
    wf.prepare(k_sample_rate, k_block_size, 1);
    const float amplitude = std::pow(10.0f, level_db / 20.0f);
    const auto output = run(wf, sine(amplitude, 1000.0f, 48000));
    return thd(output, 24000, 24000, 1000.0f);
}

}  // namespace

TEST(IntellijelWavefolder, FoldingDoesNotDependOnInputLevel) {
    for (const float drive : {1.0f, 2.0f, 5.0f}) {
        const float reference = h3_ratio(drive, 0.0f);
        for (const float level : {-30.0f, -20.0f, -10.0f}) {
            EXPECT_NEAR(h3_ratio(drive, level), reference, 0.02f + 0.05f * reference)
                << "drive " << drive << ", level " << level << " dBFS";
        }
    }
}

TEST(IntellijelWavefolder, LowDriveIsClean) {
    for (const float level : {-30.0f, -10.0f, 0.0f}) {
        EXPECT_LT(thd_at(0.3f, level), 0.01f) << "level " << level << " dBFS";
    }
}

TEST(IntellijelWavefolder, HighDriveIsClearlyFolded) {
    for (const float level : {-30.0f, -10.0f, 0.0f}) {
        EXPECT_GT(thd_at(8.0f, level), 0.5f) << "level " << level << " dBFS";
    }
}

TEST(IntellijelWavefolder, OffsetSymmetryNeverCancelsTheSignal) {
    const auto input = sine(0.1f, 220.0f, 24000);
    const auto rms = [](const std::vector<float>& x, size_t start) {
        double sum = 0.0;
        for (size_t i = start; i < x.size(); ++i) { sum += static_cast<double>(x[i]) * x[i]; }
        return static_cast<float>(std::sqrt(sum / static_cast<double>(x.size() - start)));
    };
    const float in_rms = rms(input, 12000);
    for (const float drive : {0.5f, 1.0f, std::numbers::pi_v<float>, 6.0f}) {
        for (float symmetry = -1.0f; symmetry <= 1.0f; symmetry += 0.125f) {
            TestWavefolder wf;
            wf.m_drive = drive;
            wf.m_symmetry = symmetry;
            wf.prepare(k_sample_rate, k_block_size, 1);
            const float out_rms = rms(run(wf, input), 12000);
            EXPECT_GT(out_rms, 0.3f * in_rms) << "drive " << drive << ", symmetry " << symmetry;
            EXPECT_LT(out_rms, 2.0f * in_rms) << "drive " << drive << ", symmetry " << symmetry;
        }
    }
}

TEST(IntellijelWavefolder, ToneDarkensTheFold) {
    const auto harmonic_ratio = [](float tone) {
        TestWavefolder wf;
        wf.m_drive = 8.0f;
        wf.m_tone = tone;
        wf.prepare(k_sample_rate, k_block_size, 1);
        const auto output = run(wf, sine(0.1f, 500.0f, 48000));
        return harmonic_magnitude(output, 24000, 24000, 4500.0f) /
               harmonic_magnitude(output, 24000, 24000, 500.0f);
    };
    const float open = harmonic_ratio(1.0f);
    const float dark = harmonic_ratio(0.0f);
    EXPECT_GT(open, 0.1f);
    EXPECT_LT(dark, 0.1f * open);
}
