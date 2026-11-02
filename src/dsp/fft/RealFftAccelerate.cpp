// The Accelerate (vDSP) backend of RealFft (TANH_FFT_BACKEND=accelerate, macOS and iOS).
// Accelerate is used through its umbrella header, as Apple documents it, not vDSP's own headers.
// NOLINTBEGIN(misc-include-cleaner)
#include <Accelerate/Accelerate.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/dsp/fft/RealFft.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <bit>
#include <cassert>
#include <complex>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::fft {

struct RealFft::Impl {
    explicit Impl(size_t size)
        : m_log2_size(static_cast<vDSP_Length>(std::countr_zero(size)))
        , m_setup(vDSP_create_fftsetup(m_log2_size, kFFTRadix2))
        , m_real(size / 2)
        , m_imag(size / 2)
        , m_time(size) {}
    ~Impl() { vDSP_destroy_fftsetup(m_setup); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    DSPSplitComplex split() { return {.realp = m_real.data(), .imagp = m_imag.data()}; }

    vDSP_Length m_log2_size;
    FFTSetup m_setup;
    std::vector<float> m_real;  // packed: DC in m_real[0], Nyquist in m_imag[0]
    std::vector<float> m_imag;
    std::vector<float> m_time;
};

RealFft::RealFft() = default;
RealFft::~RealFft() = default;
RealFft::RealFft(RealFft&&) noexcept = default;
RealFft& RealFft::operator=(RealFft&&) noexcept = default;

bool RealFft::prepare(size_t size) {
    m_impl.reset();
    m_size = 0;
    if (size < k_min_size || size > k_max_size || !std::has_single_bit(size)) { return false; }
    m_impl = std::make_unique<Impl>(size);
    if (m_impl->m_setup == nullptr) {
        m_impl.reset();
        return false;
    }
    m_size = size;
    return true;
}

void RealFft::forward(std::span<const float> time,
                      std::span<std::complex<float>> bins) TANH_NONBLOCKING_FUNCTION {
    assert(m_impl != nullptr && time.size() >= m_size && bins.size() >= num_bins());
    const size_t n = m_size;
    const DSPSplitComplex split = m_impl->split();
    // The even samples to the real parts, the odd ones to the imaginary parts.
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(time.data()), 2, &split, 1, n / 2);
    vDSP_fft_zrip(m_impl->m_setup, &split, 1, m_impl->m_log2_size, FFT_FORWARD);
    // vDSP's real forward transform is twice the DFT.
    bins[0] = {0.5f * split.realp[0], 0.0f};
    bins[n / 2] = {0.5f * split.imagp[0], 0.0f};
    for (size_t k = 1; k < n / 2; ++k) { bins[k] = {0.5f * split.realp[k], 0.5f * split.imagp[k]}; }
}

void RealFft::inverse(std::span<const std::complex<float>> bins,
                      std::span<float> time) TANH_NONBLOCKING_FUNCTION {
    assert(m_impl != nullptr && bins.size() >= num_bins() && time.size() >= m_size);
    const size_t n = m_size;
    const DSPSplitComplex split = m_impl->split();
    split.realp[0] = bins[0].real();
    split.imagp[0] = bins[n / 2].real();
    for (size_t k = 1; k < n / 2; ++k) {
        split.realp[k] = bins[k].real();
        split.imagp[k] = bins[k].imag();
    }
    vDSP_fft_zrip(m_impl->m_setup, &split, 1, m_impl->m_log2_size, FFT_INVERSE);
    vDSP_ztoc(&split, 1, reinterpret_cast<DSPComplex*>(m_impl->m_time.data()), 2, n / 2);
    // vDSP's real inverse transform is unscaled: N times the signal for the DFT's bins (its own
    // forward bins, twice as large, would give 2 · N).
    const float scale = 1.0f / static_cast<float>(n);
    vDSP_vsmul(m_impl->m_time.data(), 1, &scale, time.data(), 1, n);
}

const char* RealFft::backend_name() {
    return "accelerate";
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::fft
// NOLINTEND(misc-include-cleaner)
