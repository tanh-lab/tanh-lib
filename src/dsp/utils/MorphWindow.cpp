#include <tanh/core/Numbers.h>
#include <tanh/dsp/utils/MorphWindow.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace thl::dsp::utils {

namespace {

constexpr size_t k_n = MorphWindow::k_resolution;
constexpr float k_last = static_cast<float>(k_n - 1);
// The peak moves at most 95 % of the way to an edge.
constexpr float k_max_peak_shift = 0.95f;
// Below this the window counts as flat and is not tilted.
constexpr float k_flat_shape_margin = 0.02f;
constexpr float k_min_tilt = 0.005f;

using Table = std::array<float, k_n>;

Table gaussian(float sigma) {
    Table b{};
    for (size_t i = 0; i < k_n; ++i) {
        float const a = (static_cast<float>(i) - k_last * 0.5f) / (sigma * k_last * 0.5f);
        b[i] = std::exp(-0.5f * a * a);
    }
    // A short linear fade so the tails reach exactly zero.
    auto const fade = static_cast<size_t>(std::max(4.0f, std::round(sigma * 30.0f)));
    for (size_t i = 0; i < fade; ++i) {
        float const g = static_cast<float>(i) / static_cast<float>(fade);
        b[i] *= g;
        b[k_n - 1 - i] *= g;
    }
    return b;
}

Table build(WindowShape shape) {
    Table b{};
    switch (shape) {
        case WindowShape::Rectangle: b.fill(1.0f); break;
        case WindowShape::Trapezoid: {
            auto const ramp = static_cast<size_t>(std::floor(static_cast<float>(k_n) * 0.15f));
            for (size_t i = 0; i < k_n; ++i) {
                if (i < ramp) {
                    b[i] = static_cast<float>(i) / static_cast<float>(ramp);
                } else if (i >= k_n - ramp) {
                    b[i] = static_cast<float>(k_n - 1 - i) / static_cast<float>(ramp);
                } else {
                    b[i] = 1.0f;
                }
            }
            break;
        }
        case WindowShape::HalfCosine:
            for (size_t i = 0; i < k_n; ++i) {
                b[i] = std::sin(std::numbers::pi_v<float> * static_cast<float>(i) / k_last);
            }
            break;
        case WindowShape::Triangle: {
            float const mid = k_last * 0.5f;
            for (size_t i = 0; i < k_n; ++i) {
                b[i] = 1.0f - std::abs(static_cast<float>(i) - mid) / mid;
            }
            break;
        }
        case WindowShape::Hann:
            for (size_t i = 0; i < k_n; ++i) {
                b[i] = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> *
                                               static_cast<float>(i) / k_last));
            }
            break;
        case WindowShape::Gaussian: b = gaussian(0.35f); break;
        case WindowShape::Narrow: b = gaussian(0.18f); break;
        case WindowShape::Impulse: b = gaussian(0.06f); break;
        default: break;
    }
    // Peak 1, silence at both ends.
    float const peak = *std::max_element(b.begin(), b.end());
    if (peak > 0.0f) {
        for (auto& v : b) { v /= peak; }
    }
    b[0] = 0.0f;
    b[k_n - 1] = 0.0f;
    return b;
}

}  // namespace

MorphWindow::MorphWindow() {
    for (size_t s = 0; s < k_num_shapes; ++s) { m_tables[s] = build(static_cast<WindowShape>(s)); }
}

float MorphWindow::warp(float phase, float tilt) {
    // The peak (at 0.5) moves to new_mid; each half is stretched linearly
    // to meet it, so the rise and the fall keep their shape.
    float const new_mid = 0.5f * (1.0f + k_max_peak_shift * std::clamp(tilt, -1.0f, 1.0f));
    if (phase <= new_mid) { return phase / new_mid * 0.5f; }
    return 0.5f + (phase - new_mid) / (1.0f - new_mid) * 0.5f;
}

float MorphWindow::sample(const Table& table, float position) const {
    if (position <= 0.0f) { return table[0]; }
    if (position >= k_last) { return table[k_n - 1]; }
    auto const lo = static_cast<size_t>(position);
    float const frac = position - static_cast<float>(lo);
    return table[lo] * (1.0f - frac) + table[lo + 1] * frac;
}

float MorphWindow::at(float phase, float shape, float tilt) const {
    shape = std::clamp(shape, 0.0f, k_max_shape);
    auto const lo = static_cast<size_t>(shape);
    size_t const hi = std::min(lo + 1, k_num_shapes - 1);
    float const t = shape - static_cast<float>(lo);

    // A flat window is its own warp.
    bool const flat = lo == 0 && t < k_flat_shape_margin;
    float const u = (flat || std::abs(tilt) < k_min_tilt) ? phase : warp(phase, tilt);
    float const position = std::clamp(u, 0.0f, 1.0f) * k_last;
    return sample(m_tables[lo], position) * (1.0f - t) + sample(m_tables[hi], position) * t;
}

float MorphWindow::shape_at(WindowShape shape, float phase) const {
    return sample(m_tables[static_cast<size_t>(shape)], std::clamp(phase, 0.0f, 1.0f) * k_last);
}

std::string_view MorphWindow::name(WindowShape shape) {
    switch (shape) {
        case WindowShape::Rectangle: return "Rectangle";
        case WindowShape::Trapezoid: return "Trapezoid";
        case WindowShape::HalfCosine: return "Half cosine";
        case WindowShape::Triangle: return "Triangle";
        case WindowShape::Hann: return "Hann";
        case WindowShape::Gaussian: return "Gaussian";
        case WindowShape::Narrow: return "Narrow";
        case WindowShape::Impulse: return "Impulse";
        default: return "";
    }
}

const MorphWindow& MorphWindow::shared() {
    static const MorphWindow k_shared;
    return k_shared;
}

}  // namespace thl::dsp::utils
