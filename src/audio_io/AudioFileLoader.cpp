#include <tanh/audio_io/AudioFileLoader.h>
#include <tanh/core/Logger.h>
#include "DataSourceImpl.h"

#include <cstdint>
#include <vector>

namespace thl::audio_io {

namespace {

constexpr ma_uint64 READ_CHUNK_FRAMES = 4096;

dsp::audio::AudioBuffer decode_with_decoder(ma_decoder& decoder,
                                            double /*target_sample_rate*/) {
    ma_uint64 total_frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);

    auto channels = static_cast<size_t>(decoder.outputChannels);
    auto sample_rate = static_cast<double>(decoder.outputSampleRate);

    std::vector<float> interleaved;
    if (total_frames > 0) {
        interleaved.reserve(static_cast<size_t>(total_frames) * channels);
    }

    std::vector<float> chunk(READ_CHUNK_FRAMES * channels);
    while (true) {
        ma_uint64 frames_read = 0;
        ma_result result = ma_decoder_read_pcm_frames(&decoder,
                                                      chunk.data(),
                                                      READ_CHUNK_FRAMES,
                                                      &frames_read);

        if (frames_read > 0) {
            interleaved.insert(
                interleaved.end(),
                chunk.begin(),
                chunk.begin() + static_cast<ptrdiff_t>(frames_read * channels));
        }

        if (result != MA_SUCCESS || frames_read < READ_CHUNK_FRAMES) { break; }
    }

    ma_decoder_uninit(&decoder);

    if (interleaved.empty() || channels == 0) { return {}; }

    size_t num_frames = interleaved.size() / channels;

    return dsp::audio::from_interleaved(interleaved.data(),
                                        channels,
                                        num_frames,
                                        sample_rate);
}

}  // namespace

bool AudioFileLoader::init_resource_manager_data_source(
    DataSource::Impl& impl,
    const std::string& file_path,
    double target_sample_rate,
    uint32_t target_channels,
    uint32_t flags) {
    ma_resource_manager_config rm_config = ma_resource_manager_config_init();
    rm_config.decodedFormat = ma_format_f32;
    rm_config.decodedChannels = target_channels;
    rm_config.decodedSampleRate =
        target_sample_rate > 0.0 ? static_cast<ma_uint32>(target_sample_rate)
                                 : 0;

    ma_result result =
        ma_resource_manager_init(&rm_config, &impl.resourceManager);
    if (result != MA_SUCCESS) {
        thl::Logger::logf(thl::Logger::LogLevel::Error,
                          "thl.audio_io.audio_file_loader",
                          "AudioFileLoader: failed to init resource manager (error %d)",
                          static_cast<int>(result));
        return false;
    }
    impl.resourceManagerInitialised = true;

    result = ma_resource_manager_data_source_init(&impl.resourceManager,
                                                  file_path.c_str(),
                                                  flags,
                                                  nullptr,
                                                  &impl.dataSource);
    if (result != MA_SUCCESS) {
        thl::Logger::logf(thl::Logger::LogLevel::Error,
                          "thl.audio_io.audio_file_loader",
                          "AudioFileLoader: failed to init data source: %s (error %d)",
                          file_path.c_str(),
                          static_cast<int>(result));
        return false;
    }
    impl.dataSourceInitialised = true;

    impl.channels = rm_config.decodedChannels;
    impl.sampleRate = rm_config.decodedSampleRate;

    // If channels/sampleRate were left at 0 (native), query the actual
    // values from the data source after initialisation.
    if (impl.channels == 0 || impl.sampleRate == 0) {
        ma_format format;
        ma_uint32 ch = 0;
        ma_uint32 sr = 0;
        ma_data_source_get_data_format(&impl.dataSource,
                                       &format,
                                       &ch,
                                       &sr,
                                       nullptr,
                                       0);
        if (impl.channels == 0) impl.channels = ch;
        if (impl.sampleRate == 0) impl.sampleRate = sr;
    }

    return true;
}

dsp::audio::AudioBuffer AudioFileLoader::read_all_frames(
    DataSource::Impl& impl) {
    auto channels = static_cast<size_t>(impl.channels);
    auto sample_rate = static_cast<double>(impl.sampleRate);

    if (channels == 0) return {};

    ma_uint64 total_frames = 0;
    ma_data_source_get_length_in_pcm_frames(&impl.dataSource, &total_frames);

    std::vector<float> interleaved;
    if (total_frames > 0) {
        interleaved.reserve(static_cast<size_t>(total_frames) * channels);
    }

    std::vector<float> chunk(READ_CHUNK_FRAMES * channels);
    while (true) {
        ma_uint64 frames_read = 0;
        ma_result result = ma_data_source_read_pcm_frames(&impl.dataSource,
                                                          chunk.data(),
                                                          READ_CHUNK_FRAMES,
                                                          &frames_read);

        if (frames_read > 0) {
            interleaved.insert(
                interleaved.end(),
                chunk.begin(),
                chunk.begin() + static_cast<ptrdiff_t>(frames_read * channels));
        }

        if (result != MA_SUCCESS || frames_read < READ_CHUNK_FRAMES) { break; }
    }

    if (interleaved.empty()) return {};

    size_t num_frames = interleaved.size() / channels;
    return dsp::audio::from_interleaved(interleaved.data(),
                                        channels,
                                        num_frames,
                                        sample_rate);
}

dsp::audio::AudioBuffer AudioFileLoader::load_from_file(
    const std::string& file_path,
    double target_sample_rate,
    uint32_t target_channels) {
    if (file_path.empty()) return {};

    DataSource::Impl impl;
    uint32_t flags = MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE |
                     MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_WAIT_INIT;

    if (!init_resource_manager_data_source(impl,
                                           file_path,
                                           target_sample_rate,
                                           target_channels,
                                           flags)) {
        return {};
    }

    return read_all_frames(impl);
}

dsp::audio::AudioBuffer AudioFileLoader::load_from_memory(
    const void* data,
    size_t size,
    double target_sample_rate,
    uint32_t target_channels) {
    if (data == nullptr || size == 0) { return {}; }

    ma_decoder_config config = ma_decoder_config_init(
        ma_format_f32,
        target_channels > 0 ? target_channels : 0,
        target_sample_rate > 0.0 ? static_cast<ma_uint32>(target_sample_rate)
                                 : 0);

    ma_decoder decoder;
    ma_result result = ma_decoder_init_memory(data, size, &config, &decoder);

    if (result != MA_SUCCESS) {
        thl::Logger::logf(thl::Logger::LogLevel::Error,
                          "thl.audio_io.audio_file_loader",
                          "AudioFileLoader: failed to decode from memory (error %d)",
                          static_cast<int>(result));
        return {};
    }

    return decode_with_decoder(decoder, target_sample_rate);
}

DataSource AudioFileLoader::load_data_source_from_file(
    const std::string& file_path,
    double target_sample_rate,
    uint32_t target_channels) {
    auto impl = std::make_unique<DataSource::Impl>();

    if (file_path.empty()) { return DataSource(std::move(impl)); }

    uint32_t flags = MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_STREAM |
                     MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_WAIT_INIT;

    if (!init_resource_manager_data_source(*impl,
                                           file_path,
                                           target_sample_rate,
                                           target_channels,
                                           flags)) {
        return DataSource(std::make_unique<DataSource::Impl>());
    }

    return DataSource(std::move(impl));
}

DataSource AudioFileLoader::load_data_source_from_memory(
    const void* data,
    size_t size,
    double target_sample_rate,
    uint32_t target_channels) {
    auto impl = std::make_unique<DataSource::Impl>();

    if (data == nullptr || size == 0) { return DataSource(std::move(impl)); }

    ma_decoder_config config = ma_decoder_config_init(
        ma_format_f32,
        target_channels > 0 ? target_channels : 0,
        target_sample_rate > 0.0 ? static_cast<ma_uint32>(target_sample_rate)
                                 : 0);

    ma_result result =
        ma_decoder_init_memory(data, size, &config, &impl->decoder);
    if (result != MA_SUCCESS) {
        thl::Logger::logf(
            thl::Logger::LogLevel::Error,
            "thl.audio_io.audio_file_loader",
            "AudioFileLoader: failed to init decoder from memory (error %d)",
            static_cast<int>(result));
        return DataSource(std::make_unique<DataSource::Impl>());
    }
    impl->decoderInitialised = true;

    impl->channels = impl->decoder.outputChannels;
    impl->sampleRate = impl->decoder.outputSampleRate;

    return DataSource(std::move(impl));
}

}  // namespace thl::audio_io
