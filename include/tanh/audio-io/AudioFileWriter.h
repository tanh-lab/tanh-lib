#pragma once

#include <tanh/audio-io/AudioEncodingFormat.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/Exports.h>

#include <string>

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

/**
 * Encodes a planar buffer to an audio file using miniaudio.
 *
 * Samples are written as 32-bit float. The buffer is interleaved in fixed-size blocks, so long
 * buffers are never duplicated in memory.
 *
 * This class has no mutable state, so multiple instances may be used concurrently on different
 * threads.
 */
class TANH_API AudioFileWriter {
public:
    AudioFileWriter() = default;
    ~AudioFileWriter() = default;

    /**
     * Write @p buffer to @p file_path as a 32-bit float RIFF WAV file.
     *
     * The audio is first written to a uniquely named, exclusively created staging file next to
     * @p file_path and renamed over it only once every write, seek and close has succeeded, so a
     * failed export never alters an existing file.
     *
     * Returns false without touching the file system when:
     * - @p format is not WAV (the bundled miniaudio encoder only supports WAV),
     * - the channel count is 0 or above MA_MAX_CHANNELS,
     * - @p sample_rate does not round to [1, 384000] Hz, the range the bundled WAV decoder reads,
     * - the audio would exceed the 4 GiB RIFF size limit (about 23 minutes for 16 channels at
     *   48 kHz).
     *
     * @return true on success, false otherwise.
     */
    bool write_to_file(const std::string& file_path,
                       core::ConstBufferView buffer,
                       double sample_rate,
                       AudioEncodingFormat format = AudioEncodingFormat::WAV);
};

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
