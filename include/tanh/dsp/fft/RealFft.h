#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <complex>
#include <cstddef>
#include <memory>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::dsp::fft {

/**
 * @brief The FFT of a real signal of a power-of-two size, on the backend the build chose.
 *
 * forward(): size() real samples to num_bins() = size() / 2 + 1 bins,
 * X[k] = sum over n of x[n] · e^(-2πi·k·n / N), unscaled (as numpy.fft.rfft): a sine of
 * amplitude A at bin k reads |X[k]| = A · N / 2. Bin 0 (DC) and bin N / 2 (Nyquist) are real.
 * inverse(): the bins back to size() samples, scaled by 1 / N, so inverse(forward(x)) is x.
 * The imaginary parts of bins 0 and N / 2 are ignored.
 *
 * The backend is a build option (TANH_FFT_BACKEND): pffft on every platform, or Apple's
 * Accelerate on macOS and iOS; backend_name() says which. Both give the same bins.
 *
 * Threads: prepare() allocates (not on the audio thread); forward() and inverse() are real-time
 * safe and use the object's own buffers, so one object serves one thread at a time.
 */
class TANH_API RealFft {
public:
    static constexpr size_t k_min_size = 32;
    static constexpr size_t k_max_size = size_t{1} << 16;

    RealFft();
    ~RealFft();
    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;
    RealFft(RealFft&&) noexcept;
    RealFft& operator=(RealFft&&) noexcept;

    /// True for the sizes prepare() takes: powers of two in [k_min_size, k_max_size].
    [[nodiscard]] static bool is_valid_size(size_t size);

    /// Sets up the transform of @p size samples (is_valid_size()). False, and unprepared, for any
    /// other size or if the backend cannot set it up.
    bool prepare(size_t size);

    /// The prepared size (0 before prepare()).
    [[nodiscard]] size_t size() const { return m_size; }
    /// size() / 2 + 1.
    [[nodiscard]] size_t num_bins() const { return m_size == 0 ? 0 : (m_size / 2) + 1; }

    /// @p time (size() samples) to @p bins (num_bins()).
    void forward(std::span<const float> time,
                 std::span<std::complex<float>> bins) TANH_NONBLOCKING_FUNCTION;
    /// @p bins (num_bins()) to @p time (size() samples), scaled by 1 / size().
    void inverse(std::span<const std::complex<float>> bins,
                 std::span<float> time) TANH_NONBLOCKING_FUNCTION;

    /// "pffft" or "accelerate".
    [[nodiscard]] static const char* backend_name();

private:
    struct Impl;  // the backend's setup and buffers (src/dsp/fft/RealFft<Backend>.cpp)
    struct ImplDeleter {
        void operator()(Impl* impl) const;  // defined by the backend, where Impl is complete
    };
    /// The backend's setup for @p size (is_valid_size()), or nullptr if it cannot be made.
    static Impl* create_impl(size_t size);

    std::unique_ptr<Impl, ImplDeleter> m_impl;
    size_t m_size = 0;  // 0 whenever m_impl is null: also after a move
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::fft
