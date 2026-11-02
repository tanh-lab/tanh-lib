// The pffft backend of RealFft (TANH_FFT_BACKEND=pffft).
#include <pffft.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/dsp/fft/RealFft.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <complex>
#include <cstddef>
#include <memory>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::dsp::fft {

namespace {

/// Memory pffft can use with SIMD.
struct AlignedFloats {
    float* m_data = nullptr;

    explicit AlignedFloats(size_t count)
        : m_data(static_cast<float*>(pffft_aligned_malloc(count * sizeof(float)))) {}
    ~AlignedFloats() { pffft_aligned_free(m_data); }
    AlignedFloats(const AlignedFloats&) = delete;
    AlignedFloats& operator=(const AlignedFloats&) = delete;
    AlignedFloats(AlignedFloats&&) = delete;
    AlignedFloats& operator=(AlignedFloats&&) = delete;
};

struct SetupDeleter {
    void operator()(PFFFT_Setup* setup) const { pffft_destroy_setup(setup); }
};

}  // namespace

struct RealFft::Impl {
    explicit Impl(size_t size)
        : m_setup(pffft_new_setup(static_cast<int>(size), PFFFT_REAL))
        , m_input(size)
        , m_output(size)
        , m_work(size) {}

    std::unique_ptr<PFFFT_Setup, SetupDeleter> m_setup;
    AlignedFloats m_input;
    AlignedFloats m_output;
    AlignedFloats m_work;
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
    std::copy_n(time.data(), n, m_impl->m_input.m_data);
    pffft_transform_ordered(m_impl->m_setup.get(),
                            m_impl->m_input.m_data,
                            m_impl->m_output.m_data,
                            m_impl->m_work.m_data,
                            PFFFT_FORWARD);
    // Ordered real output: r(0), r(N/2), then r(k), i(k) for k = 1 .. N/2 - 1.
    const float* out = m_impl->m_output.m_data;
    bins[0] = {out[0], 0.0f};
    bins[n / 2] = {out[1], 0.0f};
    for (size_t k = 1; k < n / 2; ++k) { bins[k] = {out[2 * k], out[(2 * k) + 1]}; }
}

void RealFft::inverse(std::span<const std::complex<float>> bins,
                      std::span<float> time) TANH_NONBLOCKING_FUNCTION {
    assert(m_impl != nullptr && bins.size() >= num_bins() && time.size() >= m_size);
    const size_t n = m_size;
    float* in = m_impl->m_input.m_data;
    in[0] = bins[0].real();
    in[1] = bins[n / 2].real();
    for (size_t k = 1; k < n / 2; ++k) {
        in[2 * k] = bins[k].real();
        in[(2 * k) + 1] = bins[k].imag();
    }
    pffft_transform_ordered(m_impl->m_setup.get(),
                            in,
                            m_impl->m_output.m_data,
                            m_impl->m_work.m_data,
                            PFFFT_BACKWARD);
    // pffft's backward transform is unscaled: N times the signal.
    const float scale = 1.0f / static_cast<float>(n);
    for (size_t i = 0; i < n; ++i) { time[i] = m_impl->m_output.m_data[i] * scale; }
}

const char* RealFft::backend_name() {
    return "pffft";
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::fft
