#pragma once

#include <tanh/audio-io/WavStreamWriter.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

/// What a track of a take holds.
enum class TrackRole : uint8_t {
    Stem,  ///< One part of the mix (an engine, a return channel). export_mixdown() sums these.
    Mix    ///< The mix as it was heard while recording. Never summed into a mixdown.
};

/**
 * Where a take sits in musical time. Plain numbers: the recorder does not know the
 * transport, whoever does (dsp::transport::RecordSchedule) fills them in.
 *
 * A take that was not recorded on a grid has m_length_beats == 0.
 */
struct TakeTiming {
    double m_bpm = 0.0;            ///< Tempo at the start; 0: not known.
    int32_t m_sig_num = 4;         ///< Time signature at the start.
    int32_t m_sig_denom = 4;       ///< Time signature at the start.
    double m_start_beat = 0.0;     ///< Beat (quarter notes) of the first frame.
    double m_length_beats = 0.0;   ///< Musical length; 0: recorded off the grid.
    uint64_t m_length_frames = 0;  ///< Frames of the musical length; what follows is the tail.

    /// True when the take has a musical length.
    [[nodiscard]] constexpr bool is_musical() const noexcept { return m_length_beats > 0.0; }

    /// Quarter notes per bar of the time signature (4 for 4/4, 3 for 6/8); 0 when invalid.
    [[nodiscard]] constexpr double beats_per_bar() const noexcept {
        return m_sig_denom > 0 && m_sig_num > 0
                   ? static_cast<double>(m_sig_num) * 4.0 / static_cast<double>(m_sig_denom)
                   : 0.0;
    }

    /// The musical length in bars; 0 off the grid.
    [[nodiscard]] constexpr double length_bars() const noexcept {
        const double bar = beats_per_bar();
        return bar > 0.0 ? m_length_beats / bar : 0.0;
    }
};

static_assert(std::is_trivially_copyable_v<TakeTiming>);

/// One track of a take: one audio file.
struct TakeTrack {
    std::string m_name;       ///< As given to the recorder.
    std::string m_file;       ///< File name inside the take's directory.
    uint32_t m_channels = 2;  ///< Channels of the file.
    TrackRole m_role = TrackRole::Stem;
};

/**
 * A finished recording: a directory with one WAV file per track, all of the same length and
 * starting on the same sample, and a manifest (`take.json`) that says what they are.
 *
 * MultitrackRecorder writes one; load_take() reads it back, so a take can be exported or
 * remixed in a later session.
 */
struct Take {
    /// File name of the manifest inside the directory.
    static constexpr const char* k_manifest_name = "take.json";

    std::string m_directory;
    double m_sample_rate = 0.0;
    SampleFormat m_format = SampleFormat::Float32;
    uint64_t m_num_frames = 0;      ///< Length of every file.
    uint64_t m_dropped_frames = 0;  ///< Frames the disk was too slow for, written as silence.
    bool m_truncated = false;       ///< Ended early because a file reached its size limit.
    TakeTiming m_timing;
    std::vector<TakeTrack> m_tracks;

    /// Frames of the musical length (the loop), without the tail; the whole take when it has
    /// no musical length.
    [[nodiscard]] uint64_t musical_frames() const noexcept {
        const uint64_t frames = m_timing.m_length_frames;
        return m_timing.is_musical() && frames > 0 && frames < m_num_frames ? frames : m_num_frames;
    }

    /// Path of the file of track @p track; empty for an index out of range.
    [[nodiscard]] TANH_API std::string track_path(size_t track) const;
};

/// Writes @p take's manifest into its directory (staged and renamed, so a reader never sees
/// half of one). False when the directory is missing or the file system refuses.
[[nodiscard]] TANH_API bool save_take(const Take& take);

/// Reads the manifest in @p directory. Empty when there is none or it cannot be understood;
/// the audio files are not opened.
[[nodiscard]] TANH_API std::optional<Take> load_take(const std::string& directory);

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
