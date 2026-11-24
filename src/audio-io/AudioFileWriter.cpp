#include <tanh/audio-io/AudioEncodingFormat.h>
#include <tanh/audio-io/AudioFileWriter.h>
#include <tanh/audio-io/WavStreamWriter.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

bool AudioFileWriter::write_to_file(const std::string& file_path,
                                    const core::ConstBufferView& buffer,
                                    double sample_rate,
                                    AudioEncodingFormat format) {
    if (format != AudioEncodingFormat::WAV) { return false; }

    // The size is known up front: refuse what cannot fit before a staging file exists.
    const size_t num_channels = buffer.get_num_channels();
    if (num_channels == 0 || num_channels > UINT32_MAX) { return false; }
    const auto channels = static_cast<uint32_t>(num_channels);
    if (buffer.get_num_samples() > WavStreamWriter::max_frames(channels, SampleFormat::Float32)) {
        return false;
    }

    WavStreamWriter writer;
    if (!writer.open(file_path,
                     {.m_channels = channels,
                      .m_sample_rate = sample_rate,
                      .m_format = SampleFormat::Float32})) {
        return false;
    }
    if (!writer.write(buffer)) {
        writer.abort();
        return false;
    }
    return writer.close();
}

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
