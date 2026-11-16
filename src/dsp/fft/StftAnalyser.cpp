#include <tanh/core/AbiNamespace.h>
#include <tanh/dsp/fft/RealFft.h>
#include <tanh/dsp/fft/StftAnalyser.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::fft {

namespace {

/// The periodic window of @p size (DFT-even: its spectrum has no scalloping offset).
std::vector<float> make_window(StftAnalyser::Window window, size_t size) {
    std::vector<float> w(size);
    for (size_t i = 0; i < size; ++i) {
        const double x =
            2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(size);
        double value = 0.0;
        if (window == StftAnalyser::Window::Hann) {
            value = 0.5 - (0.5 * std::cos(x));
        } else {  // 4-term Blackman-Harris (Harris 1978): -92 dB sidelobes
            value = 0.35875 - (0.48829 * std::cos(x)) + (0.14128 * std::cos(2.0 * x)) -
                    (0.01168 * std::cos(3.0 * x));
        }
        w[i] = static_cast<float>(value);
    }
    // A sine of amplitude A at a bin's centre gives |X| = A · sum(w) / 2: scale that away.
    double sum = 0.0;
    for (const float v : w) { sum += v; }
    const auto scale = static_cast<float>(2.0 / sum);
    for (float& v : w) { v *= scale; }
    return w;
}

}  // namespace

StftAnalyser::~StftAnalyser() = default;

void StftAnalyser::clear() {
    m_fft = RealFft();
    m_hop_size = 0;
    m_filled = 0;
    m_channels = {};
    m_window = {};
    m_frame = {};
    m_bins = {};
}

bool StftAnalyser::prepare(size_t fft_size, size_t hop_size, size_t num_channels, Window window) {
    // Checked before anything is allocated; a rejected prepare() leaves it unprepared.
    if (!RealFft::is_valid_size(fft_size) || hop_size == 0 || hop_size > fft_size) {
        clear();
        return false;
    }
    // Built aside and taken over at the end: if an allocation throws, it stays as it was.
    RealFft fft;
    if (!fft.prepare(fft_size)) {
        clear();
        return false;
    }
    std::vector<std::vector<float>> channels(num_channels, std::vector<float>(fft_size, 0.0f));
    std::vector<float> window_values = make_window(window, fft_size);
    std::vector<float> frame(fft_size, 0.0f);
    std::vector<std::complex<float>> bins(fft.num_bins());

    m_fft = std::move(fft);
    m_hop_size = hop_size;
    m_filled = 0;
    m_channels = std::move(channels);
    m_window = std::move(window_values);
    m_frame = std::move(frame);
    m_bins = std::move(bins);
    return true;
}

void StftAnalyser::reset() TANH_NONBLOCKING_FUNCTION {
    m_filled = 0;
}

void StftAnalyser::push(std::span<const float* const> channels,
                        size_t num_samples) TANH_NONBLOCKING_FUNCTION {
    const size_t n = fft_size();
    if (n == 0 || m_hop_size == 0) { return; }
    size_t offset = 0;
    while (offset < num_samples) {
        // Up to the next frame: the buffers are full at n.
        const size_t count = std::min(num_samples - offset, n - m_filled);
        for (size_t ch = 0; ch < m_channels.size(); ++ch) {
            float* buffer = m_channels[ch].data() + m_filled;
            const float* source =
                ch < channels.size() && channels[ch] != nullptr ? channels[ch] + offset : nullptr;
            if (source != nullptr) {
                std::copy_n(source, count, buffer);
            } else {
                std::fill_n(buffer, count, 0.0f);
            }
        }
        m_filled += count;
        offset += count;
        if (m_filled == n) {
            run_frame();
            // Keep the newest n - hop samples: the next frame comes after hop more.
            for (auto& buffer : m_channels) {
                std::copy(buffer.begin() + static_cast<std::ptrdiff_t>(m_hop_size),
                          buffer.end(),
                          buffer.begin());
            }
            m_filled = n - m_hop_size;
        }
    }
}

void StftAnalyser::run_frame() TANH_NONBLOCKING_FUNCTION {
    for (size_t ch = 0; ch < m_channels.size(); ++ch) {
        const std::vector<float>& buffer = m_channels[ch];
        for (size_t i = 0; i < m_frame.size(); ++i) { m_frame[i] = buffer[i] * m_window[i]; }
        m_fft.forward(m_frame, m_bins);
        // DC and Nyquist have no mirror image: the window's factor 2 does not apply to them.
        m_bins.front() *= 0.5f;
        m_bins.back() *= 0.5f;
        process_spectrum(ch, m_bins);
    }
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::fft
