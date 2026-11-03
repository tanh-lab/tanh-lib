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
     * Write @p buffer to @p file_path, replacing any existing file.
     *
     * @return true on success, false if the file could not be created or written.
     */
    bool write_to_file(const std::string& file_path,
                       core::ConstBufferView buffer,
                       double sample_rate,
                       AudioEncodingFormat format = AudioEncodingFormat::WAV);
};

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
