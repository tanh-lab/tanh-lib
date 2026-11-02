#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/fft/RealFft.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::fft {

/**
 * @brief Cuts a stream of samples into overlapping frames and hands each frame's spectrum to
 * process_spectrum(): the analysis half of a short-time Fourier transform.
 *
 * push() takes samples in chunks of any size (as they come from a SampleProbe). Every hop_size()
 * samples, once fft_size() have arrived, each channel's newest fft_size() samples are windowed,
 * transformed (RealFft) and handed to process_spectrum(), channel by channel, the same frame for
 * every channel. The bins are scaled so a sine of amplitude A reads |X| = A at its bin: the
 * window's coherent gain and the FFT's N / 2 undone, so a full-scale sine reads 0 dB.
 *
 * reset() drops what has been collected: the next frame comes only after fft_size() new samples
 * (after a discontinuity, so no frame spans a gap).
 *
 * Threads: prepare() allocates; push() and reset() allocate nothing and belong to one thread at a
 * time (a reader thread, not necessarily the audio thread).
 */
class TANH_API StftAnalyser {
public:
    enum class Window : uint8_t {
        Hann,            ///< the usual: -31 dB sidelobes, narrow main lobe
        BlackmanHarris,  ///< 4-term: -92 dB sidelobes for a high dynamic range, wider main lobe
    };

    StftAnalyser() = default;
    virtual ~StftAnalyser();
    StftAnalyser(const StftAnalyser&) = delete;
    StftAnalyser& operator=(const StftAnalyser&) = delete;

    /// Frames of @p fft_size (a power of two RealFft takes), one every @p hop_size samples
    /// (1 to fft_size), for @p num_channels channels. False, and unprepared, otherwise.
    bool prepare(size_t fft_size,
                 size_t hop_size,
                 size_t num_channels,
                 Window window = Window::Hann);

    [[nodiscard]] size_t fft_size() const { return m_fft.size(); }
    [[nodiscard]] size_t hop_size() const { return m_hop_size; }
    [[nodiscard]] size_t num_bins() const { return m_fft.num_bins(); }
    [[nodiscard]] size_t num_channels() const { return m_channels.size(); }

    /// Feeds @p num_samples of every channel (@p channels[c]: channel c; a missing or null one
    /// reads as silence). Calls process_spectrum() for every frame that completes.
    void push(std::span<const float* const> channels, size_t num_samples) TANH_NONBLOCKING_FUNCTION;

    /// Drops what has been collected: the next frame needs fft_size() new samples.
    void reset() TANH_NONBLOCKING_FUNCTION;

protected:
    /// One frame of channel @p channel: num_bins() bins, scaled as described above. Called for
    /// every channel of a frame in turn.
    virtual void process_spectrum(size_t channel, std::span<const std::complex<float>> bins)
        TANH_NONBLOCKING_FUNCTION = 0;

private:
    void run_frame() TANH_NONBLOCKING_FUNCTION;

    RealFft m_fft;
    size_t m_hop_size = 0;
    size_t m_filled = 0;                         // samples in every channel's frame buffer
    std::vector<std::vector<float>> m_channels;  // per channel: the newest m_filled samples
    std::vector<float> m_window;                 // scaled: undoes its gain and the FFT's N / 2
    std::vector<float> m_frame;
    std::vector<std::complex<float>> m_bins;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::fft
