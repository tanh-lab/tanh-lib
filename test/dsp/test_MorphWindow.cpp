#include <gtest/gtest.h>
#include <tanh/core/Numbers.h>
#include <tanh/dsp/utils/MorphWindow.h>

#include <cmath>
#include <cstddef>

using thl::dsp::utils::MorphWindow;
using thl::dsp::utils::WindowShape;

namespace {
const MorphWindow& w() {
    return MorphWindow::shared();
}
constexpr float k_tol = 2e-3f;  // 512-point tables, linear interpolation
}  // namespace

TEST(MorphWindow, IntegerShapesLandExactlyOnTheirFormulas) {
    // Hann
    EXPECT_NEAR(w().at(0.5f, 4.0f, 0.0f), 1.0f, k_tol);
    EXPECT_NEAR(w().at(0.25f, 4.0f, 0.0f), 0.5f, k_tol);
    // Half cosine
    EXPECT_NEAR(w().at(0.5f, 2.0f, 0.0f), 1.0f, k_tol);
    EXPECT_NEAR(w().at(0.25f, 2.0f, 0.0f), std::sin(std::numbers::pi_v<float> * 0.25f), k_tol);
    // Triangle
    EXPECT_NEAR(w().at(0.25f, 3.0f, 0.0f), 0.5f, k_tol);
    EXPECT_NEAR(w().at(0.5f, 3.0f, 0.0f), 1.0f, k_tol);
    // Rectangle: flat everywhere inside
    EXPECT_NEAR(w().at(0.1f, 0.0f, 0.0f), 1.0f, k_tol);
    EXPECT_NEAR(w().at(0.9f, 0.0f, 0.0f), 1.0f, k_tol);
    // Trapezoid: 15 % ramps
    EXPECT_NEAR(w().at(0.075f, 1.0f, 0.0f), 0.5f, 0.01f);  // 76-sample ramp
    EXPECT_NEAR(w().at(0.5f, 1.0f, 0.0f), 1.0f, k_tol);
    // Gaussians peak at the centre and get narrower
    EXPECT_NEAR(w().at(0.5f, 5.0f, 0.0f), 1.0f, k_tol);
    EXPECT_GT(w().at(0.35f, 5.0f, 0.0f), w().at(0.35f, 6.0f, 0.0f));
    EXPECT_GT(w().at(0.35f, 6.0f, 0.0f), w().at(0.35f, 7.0f, 0.0f));
}

TEST(MorphWindow, EveryShapeStartsAndEndsInSilenceAndPeaksAtOne) {
    for (size_t s = 0; s < MorphWindow::k_num_shapes; ++s) {
        auto const shape = static_cast<float>(s);
        EXPECT_FLOAT_EQ(w().at(0.0f, shape, 0.0f), 0.0f) << "shape " << s;
        EXPECT_FLOAT_EQ(w().at(1.0f, shape, 0.0f), 0.0f) << "shape " << s;
        float peak = 0.0f;
        for (int i = 0; i < 2000; ++i) {
            peak = std::max(peak, w().at(static_cast<float>(i) / 2000.0f, shape, 0.0f));
        }
        EXPECT_NEAR(peak, 1.0f, 1e-4f) << "shape " << s;
    }
}

TEST(MorphWindow, FractionalShapeIsTheLinearBlendOfItsNeighbours) {
    for (float phase : {0.1f, 0.3f, 0.5f, 0.8f}) {
        float const a = w().at(phase, 3.0f, 0.0f);
        float const b = w().at(phase, 4.0f, 0.0f);
        EXPECT_NEAR(w().at(phase, 3.5f, 0.0f), 0.5f * (a + b), 1e-6f);
        EXPECT_NEAR(w().at(phase, 3.25f, 0.0f), 0.75f * a + 0.25f * b, 1e-6f);
    }
}

TEST(MorphWindow, TiltMovesThePeakAndKeepsTheEndsSilent) {
    auto peak_phase = [](float shape, float tilt) {
        float best = 0.0f;
        float at_best = -1.0f;
        for (int i = 0; i <= 4000; ++i) {
            float const phase = static_cast<float>(i) / 4000.0f;
            float const v = w().at(phase, shape, tilt);
            if (v > at_best) {
                at_best = v;
                best = phase;
            }
        }
        return best;
    };
    // Hann: peak at 0.5 (1 + 0.95 tilt)
    // The Hann table tops out over two samples, so the argmax has ~2e-3 play.
    EXPECT_NEAR(peak_phase(4.0f, 0.0f), 0.5f, 5e-3f);
    EXPECT_NEAR(peak_phase(4.0f, 0.5f), 0.7375f, 5e-3f);
    EXPECT_NEAR(peak_phase(4.0f, -1.0f), 0.025f, 5e-3f);
    EXPECT_NEAR(peak_phase(4.0f, 1.0f), 0.975f, 5e-3f);
    // Tilt 0 is the untilted window exactly.
    for (float phase : {0.1f, 0.4f, 0.6f, 0.9f}) {
        EXPECT_FLOAT_EQ(w().at(phase, 4.0f, 0.0f), w().at(phase, 4.0f, 0.004f));
    }
    // Ends stay silent under any tilt.
    EXPECT_FLOAT_EQ(w().at(0.0f, 4.0f, 0.8f), 0.0f);
    EXPECT_FLOAT_EQ(w().at(1.0f, 4.0f, -0.8f), 0.0f);
    // The warp is continuous at the peak.
    float const new_mid = 0.5f * (1.0f + 0.95f * 0.6f);
    EXPECT_NEAR(w().at(new_mid - 1e-4f, 4.0f, 0.6f), w().at(new_mid + 1e-4f, 4.0f, 0.6f), 2e-3f);
}

TEST(MorphWindow, RectangleIgnoresTilt) {
    for (float phase : {0.05f, 0.3f, 0.7f, 0.95f}) {
        EXPECT_FLOAT_EQ(w().at(phase, 0.0f, 1.0f), w().at(phase, 0.0f, 0.0f));
        EXPECT_FLOAT_EQ(w().at(phase, 0.01f, -1.0f), w().at(phase, 0.01f, 0.0f));
    }
    // Just past the flat margin the tilt applies again.
    EXPECT_NE(w().at(0.1f, 0.5f, 1.0f), w().at(0.1f, 0.5f, 0.0f));
}

TEST(MorphWindow, NamesAndWarp) {
    EXPECT_EQ(MorphWindow::name(WindowShape::Hann), "Hann");
    EXPECT_EQ(MorphWindow::name(WindowShape::Impulse), "Impulse");
    EXPECT_FLOAT_EQ(MorphWindow::warp(0.5f, 0.0f), 0.5f);
    EXPECT_FLOAT_EQ(MorphWindow::warp(0.0f, 0.7f), 0.0f);
    EXPECT_FLOAT_EQ(MorphWindow::warp(1.0f, 0.7f), 1.0f);
    // At tilt +1 the source mid maps to output 0.975.
    EXPECT_NEAR(MorphWindow::warp(0.975f, 1.0f), 0.5f, 1e-6f);
}
