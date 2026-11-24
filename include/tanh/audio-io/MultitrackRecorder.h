#pragma once

#include <tanh/audio-io/Take.h>
#include <tanh/audio-io/WavStreamWriter.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

/// One track a MultitrackRecorder records.
struct RecorderTrack {
    /// Name of the track and, with unsafe characters replaced, of its file.
    std::string m_name;
    uint32_t m_channels = 2;
    TrackRole m_role = TrackRole::Stem;
};

/// What a MultitrackRecorder is prepared for: the most it will be asked to record. Which of
/// the tracks a take records is chosen per take (MultitrackRecorder::begin_take()).
struct RecorderConfig {
    double m_sample_rate = 48000.0;
    /// Largest block the audio thread will hand over.
    uint32_t m_max_block_size = 1024;
    /// Audio the recorder holds per track between the audio thread and the disk. A disk that
    /// stalls for longer costs frames (written as silence, counted).
    double m_buffer_seconds = 2.0;
    /// Every track that can be recorded. Each one holds its buffer from prepare() on (about
    /// 1 MB for a stereo track at 48 kHz and two seconds), recorded or not.
    std::vector<RecorderTrack> m_tracks;
};

/// The part of a block that belongs to the take.
struct RecordSpan {
    uint32_t m_begin = 0;  ///< First sample of the block that is recorded.
    uint32_t m_end = 0;    ///< One past the last; equal to m_begin: nothing of this block.
    bool m_final = false;  ///< The take ends at m_end.
};

/// Where a MultitrackRecorder stands.
enum class RecorderState : uint8_t {
    Idle,       ///< No take.
    Ready,      ///< A take is open, no audio has arrived yet.
    Recording,  ///< Audio is arriving.
    Finishing,  ///< The take has ended; the rest is being written and the files closed.
    Done,       ///< The take is on disk: last_take().
    Failed      ///< The take could not be written. Its files are removed, unless only the
                ///< manifest failed: then the audio files are complete and stay.
};

/**
 * Records several tracks at once into one WAV file each, so that the files line up to the
 * sample: every engine, every return channel and the mix, for instance.
 *
 * The audio thread only copies: every block goes into a preallocated buffer per track, and
 * one writer thread of the recorder moves the buffers to disk. The audio thread never
 * touches the file system, takes no lock and does not allocate.
 *
 * @par One block
 * @code
 * recorder.begin_block(num_samples, span);        // which samples belong to the take
 * recorder.write(0, engine_a);                    // wherever each signal is at hand
 * recorder.write(1, engine_b);
 * recorder.write(2, mix);
 * recorder.end_block();
 * @endcode
 * Call them every block, whether a take is open or not: without one they do nothing, and
 * the end of a take may need the block after its last one to get through. A track that was not
 * written in a block gets silence for it, and channels a block lacks are silent, so the tracks stay
 * the same length whatever happens.
 *
 * @par One take
 * begin_take() creates the directory and the files and waits for audio. It also says which of
 * the prepared tracks this take records (all of them, only the mix, ...), so that is a choice
 * per take and needs no prepare(). The take ends at the
 * sample a span marks as final (dsp::transport::RecordSchedule puts that on a bar line), or
 * when end_take() is called; the writer thread then writes what is left, closes the files and
 * writes the manifest, after which state() is Done and last_take() describes what is on disk.
 * cancel_take() drops the take and its files (only its own: what was in the directory before
 * stays).
 *
 * @par When the disk is too slow
 * A block that does not fit the buffers is left out of every track and written as silence in
 * every file (dropped_frames()), so the tracks stay aligned and the take keeps its length.
 *
 * @par When a file is full
 * WAV files hold 4 GiB. A take that reaches the limit ends there and is marked truncated.
 *
 * Threading: prepare() with the audio stopped; begin_block(), write(), set_timing() and
 * end_block() on the audio thread; everything else on one control thread. The completion
 * callback runs on the writer thread.
 */
class TANH_API MultitrackRecorder {
public:
    /// Called on the writer thread when a take is on disk (@p ok) or could not be written.
    using TakeCallback = std::function<void(const Take& take, bool ok)>;

    MultitrackRecorder();
    /// Cancels a take that is still open.
    ~MultitrackRecorder();

    MultitrackRecorder(const MultitrackRecorder&) = delete;
    MultitrackRecorder& operator=(const MultitrackRecorder&) = delete;

    /// Allocates for @p config. Cancels a take that is still open. False (and nothing is
    /// prepared) for no tracks, a track without channels or an unusable sample rate.
    bool prepare(const RecorderConfig& config);

    /// The configuration of the last successful prepare().
    [[nodiscard]] const RecorderConfig& config() const noexcept;

    /// Set while no take is open; replaces the previous one.
    void set_take_callback(TakeCallback callback);

    /**
     * Opens a take: creates @p directory (and its parents) and one file per recorded track in
     * it, written in @p format.
     *
     * @p tracks picks the tracks of this take by their index in RecorderConfig::m_tracks:
     * only the mix, for instance, or every stem. Empty (the default) records all of them.
     * The others cost nothing during the take: no file, no copying. write() keeps taking the
     * same indices either way, so the audio thread does not need to know the choice.
     *
     * False when a take is still open (Ready, Recording, Finishing), the recorder is not
     * prepared, an index is out of range, or the directory or a file cannot be created.
     */
    bool begin_take(const std::string& directory,
                    SampleFormat format = SampleFormat::Float32,
                    const std::vector<size_t>& tracks = {});

    /**
     * Ends the open take with what has arrived, for a take that no span marked as final (a
     * stop that is not on a grid, or an audio device that stopped). @p timing is what the
     * manifest gets. Returns at once; state() says when the files are closed. Does nothing
     * without an open take or after a final span.
     */
    void end_take(const TakeTiming& timing = {});

    /// Drops the open take, also one whose end is still being written: afterwards state() is
    /// Idle and nothing of the take is on disk. Files that were in the directory before the
    /// take stay. Does nothing for a take that is already Done. Waits for the writer thread.
    void cancel_take();

    /// Waits until the open take is Done or Failed (or @p timeout_seconds have passed), for
    /// tests and for shutting down. True when no take is being written any more.
    bool wait_until_finished(double timeout_seconds = 10.0);

    /// Where the recorder stands. Any thread.
    [[nodiscard]] RecorderState state() const noexcept;
    /// Frames of the open or last take so far, silence for dropped blocks included. Any thread.
    [[nodiscard]] uint64_t frames_recorded() const noexcept;
    /// Frames of the open or last take the buffers had no room for. Any thread.
    [[nodiscard]] uint64_t dropped_frames() const noexcept;
    /// The last take that reached Done; empty before the first.
    [[nodiscard]] std::optional<Take> last_take() const;

    // ---- audio thread ----------------------------------------------------------------------

    /// Starts a block of @p num_samples samples of which @p span belongs to the take.
    void begin_block(uint32_t num_samples, RecordSpan span) noexcept TANH_NONBLOCKING_FUNCTION;

    /// The whole block belongs to the take: for recording without a grid.
    void begin_block(uint32_t num_samples) noexcept TANH_NONBLOCKING_FUNCTION {
        begin_block(num_samples, {.m_begin = 0, .m_end = num_samples, .m_final = false});
    }

    /// Hands over the block of track @p track (its index in RecorderConfig::m_tracks); it
    /// must hold the block's samples. Once per track and block: a second call for the same
    /// track is ignored, and so is a track the take does not record.
    void write(size_t track, const core::ConstBufferView& block) noexcept TANH_NONBLOCKING_FUNCTION;

    /// The musical facts of the take, for the manifest. Call before the end_block() of the
    /// block whose span is final.
    void set_timing(const TakeTiming& timing) noexcept TANH_NONBLOCKING_FUNCTION;

    /// Ends the block: tracks nobody wrote get silence, and a final span ends the take.
    void end_block() noexcept TANH_NONBLOCKING_FUNCTION;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
