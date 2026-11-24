#pragma once

#include <tanh/audio-io/Take.h>
#include <tanh/audio-io/WavStreamWriter.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

/// What an export writes.
struct ExportOptions {
    SampleFormat m_format = SampleFormat::Pcm24;
    /// Pcm16 only, see WavStreamConfig::m_dither.
    bool m_dither = true;

    /// First frame of the take that is exported.
    uint64_t m_start_frame = 0;
    /// Frames to export; 0: up to the end of the take. Take::musical_frames() gives the
    /// length without the tail, for a file that loops.
    uint64_t m_num_frames = 0;

    /// Scales the result so that its highest peak sits at this level in dBFS (0 or below).
    /// Empty: levels stay as recorded. export_tracks() ignores it, so the files keep their
    /// balance.
    std::optional<float> m_normalize_db;

    /// export_mixdown(): linear gain per track of the take, by index; tracks beyond the list
    /// play at 1, a gain of 0 mutes.
    std::vector<float> m_track_gains;

    /// Called with the progress, 0 to 1, on the thread that runs the export; false cancels.
    std::function<bool(float)> m_progress;
};

/// How an export ended.
enum class ExportResult : uint8_t {
    Done,       ///< The file is at its path.
    Cancelled,  ///< m_progress returned false; nothing was written.
    Failed      ///< A track could not be read or the file not written; nothing was written.
};

/**
 * Mixes the stems of @p take (TrackRole::Stem; the recorded mix is left out) into one file.
 *
 * The file has as many channels as the widest stem. A mono stem goes to every channel,
 * any other stem to its own channels.
 *
 * All three exports read and write block by block, so a long take is never in memory at once,
 * and publish the file only when it is complete. They block: run them on a worker thread.
 */
[[nodiscard]] TANH_API ExportResult export_mixdown(const Take& take,
                                                   const std::string& file_path,
                                                   const ExportOptions& options = {});

/// Writes track @p track of @p take (a stem, or the mix as it was recorded) to @p file_path,
/// in the format and range of @p options.
[[nodiscard]] TANH_API ExportResult export_track(const Take& take,
                                                 size_t track,
                                                 const std::string& file_path,
                                                 const ExportOptions& options = {});

/// Writes every track of @p take into @p directory (created when missing) under its file
/// name, all with the same range, so they still line up. The files appear together when the
/// last one is complete; a cancelled or failed export leaves the directory as it was. The
/// take's own directory is refused.
[[nodiscard]] TANH_API ExportResult export_tracks(const Take& take,
                                                  const std::string& directory,
                                                  const ExportOptions& options = {});

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
