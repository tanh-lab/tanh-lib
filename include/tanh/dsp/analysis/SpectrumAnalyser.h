#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/dsp/fft/StftAnalyser.h>

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::dsp::probe {
class SampleProbe;
}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::probe

namespace thl::inline THL_ABI_NAMESPACE::dsp::analysis {

/**
 * @brief The curve of a spectrum analyser display: an EQ's analyser, a mastering meter.
 *
 * Frames come from an StftAnalyser (fed with push(), or drained from a SampleProbe with
 * read_from()). Every frame makes one curve of num_points() values in dB, at frequencies()
 * spaced evenly on a log axis from min_frequency to max_frequency:
 * - The channels are averaged in power (a stereo signal reads as its mean).
 * - Without smoothing, a point is the largest bin between it and its neighbours (a sine keeps
 *   its level however many bins fall on one point); where a point lies between two bins, the
 *   power between them. With fractional-octave smoothing, a point is the mean power over its
 *   band (a sine is spread over it).
 * - Tilt: tilt_db_per_octave added per octave around 1 kHz; 4.5 dB/octave (the default) is how
 *   the ear weighs loudness, and shows pink noise about flat.
 * - Speed: a point rises at once and falls by release_db_per_second at most (the display's
 *   ballistics). Freeze: it does not fall at all, so the curve holds the maximum.
 * - Below floor_db everything reads floor_db (the display's range).
 * Scaled as StftAnalyser: a full-scale sine at a bin's frequency reads 0 dB, tilt 0.
 *
 * Threads: one reader thread (a UI timer, an analysis thread), not the audio thread.
 * prepare() and set_resolution() allocate; push(), read_from(), the other setters and curve_db()
 * do not.
 */
class TANH_API SpectrumAnalyser : public thl::dsp::fft::StftAnalyser {
public:
    /// The FFT size: Low 1024, Medium 2048, High 4096 (the default), Maximum 8192 points. More
    /// points: finer at low frequencies, but fewer frames a second (a slower attack).
    enum class Resolution : uint8_t { Low, Medium, High, Maximum };

    struct Settings {
        Resolution m_resolution = Resolution::High;
        Window m_window = Window::Hann;  ///< BlackmanHarris for a range beyond 90 dB
        size_t m_num_points = 256;
        float m_min_frequency = 20.0f;
        float m_max_frequency = 20000.0f;
        float m_tilt_db_per_octave = 4.5f;
        float m_smoothing_octaves = 0.0f;       ///< 0 off, else e.g. 1/3, 1/6, 1/12, 1/24
        float m_release_db_per_second = 30.0f;  ///< the speed
        float m_floor_db = -120.0f;
        bool m_freeze = false;
    };

    /// Frames of the resolution's size, four a frame (75 % overlap), for @p num_channels channels
    /// at @p sample_rate. False, and unprepared, for settings it cannot use.
    bool prepare(double sample_rate, size_t num_channels, const Settings& settings);
    /// prepare() with the default Settings.
    bool prepare(double sample_rate, size_t num_channels);

    [[nodiscard]] const Settings& settings() const { return m_settings; }

    /// Re-prepares at another FFT size (allocates); the curve starts over.
    bool set_resolution(Resolution resolution);
    void set_tilt(float db_per_octave);
    void set_smoothing(float octaves);
    void set_release(float db_per_second);
    /// Frozen, the curve only rises; unfrozen it falls again from the maximum it held.
    void set_freeze(bool freeze) { m_settings.m_freeze = freeze; }
    void set_floor(float floor_db);

    /// Drains @p probe (its one reader) and feeds what it held. After a discontinuity the frames
    /// start over (no frame spans the gap); the curve goes on falling from where it was.
    void read_from(thl::dsp::probe::SampleProbe& probe);

    /// The frequency (Hz) of every point.
    [[nodiscard]] std::span<const float> frequencies() const { return m_frequencies; }
    /// The curve in dB, as of the last frame (floor_db() before the first).
    [[nodiscard]] std::span<const float> curve_db() const { return m_curve_db; }
    /// Frames made since prepare().
    [[nodiscard]] size_t num_frames() const { return m_num_frames; }

protected:
    void process_spectrum(size_t channel, std::span<const std::complex<float>> bins) override;

private:
    struct Point {
        // A band of bins, [m_first, m_last], and for a point between two bins the bins and the
        // weight of the upper one (m_interpolate).
        size_t m_first = 0;
        size_t m_last = 0;
        bool m_interpolate = false;
        float m_upper_weight = 0.0f;
        float m_tilt_db = 0.0f;
    };

    [[nodiscard]] static size_t fft_size_of(Resolution resolution);
    void update_points();
    void make_curve();

    double m_sample_rate = 48000.0;
    size_t m_num_channels = 0;
    Settings m_settings;
    std::vector<float> m_frequencies;
    std::vector<Point> m_points;
    std::vector<float> m_power;     // per bin, summed over the channels of the frame
    std::vector<float> m_curve_db;  // per point
    std::vector<float> m_scratch;   // read_from(): one chunk per channel
    std::vector<float*> m_scratch_pointers;
    float m_frame_seconds = 0.0f;  // the time between frames, for the release
    size_t m_num_frames = 0;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::analysis
