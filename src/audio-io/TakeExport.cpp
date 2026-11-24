#include <tanh/audio-io/Take.h>
#include <tanh/audio-io/TakeExport.h>
#include <tanh/audio-io/WavStreamWriter.h>
#include <tanh/core/AbiNamespace.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "miniaudio.h"

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

namespace {

constexpr size_t k_chunk_frames = 4096;

// A file of the take, decoded on this thread. (The loader's streaming DataSource decodes on a
// job thread and hands out silence while a page is not there yet: right for playback, wrong
// for an export, which waits for every sample.)
class Decoder {
public:
    Decoder() = default;
    ~Decoder() { close(); }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool open(const std::string& path) {
        close();
        const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        m_open = ma_decoder_init_file(path.c_str(), &config, &m_decoder) == MA_SUCCESS;
        return m_open;
    }

    void close() {
        if (m_open) { ma_decoder_uninit(&m_decoder); }
        m_open = false;
    }

    [[nodiscard]] uint32_t channels() const { return m_decoder.outputChannels; }
    [[nodiscard]] uint32_t sample_rate() const { return m_decoder.outputSampleRate; }

    bool seek(uint64_t frame) {
        return ma_decoder_seek_to_pcm_frame(&m_decoder, frame) == MA_SUCCESS;
    }

    // The frames read; fewer than asked for at the end of the file.
    size_t read(float* interleaved, size_t frames) {
        ma_uint64 frames_read = 0;
        ma_decoder_read_pcm_frames(&m_decoder, interleaved, frames, &frames_read);
        return static_cast<size_t>(frames_read);
    }

private:
    ma_decoder m_decoder{};
    bool m_open = false;
};

// One file that goes into the result.
struct Source {
    std::unique_ptr<Decoder> m_decoder;
    uint32_t m_channels = 0;
    float m_gain = 1.0f;
};

struct Range {
    uint64_t m_start = 0;
    uint64_t m_frames = 0;
};

Range range_of(const Take& take, const ExportOptions& options) {
    const uint64_t start = std::min(options.m_start_frame, take.m_num_frames);
    const uint64_t available = take.m_num_frames - start;
    const uint64_t frames =
        options.m_num_frames == 0 ? available : std::min(options.m_num_frames, available);
    return {.m_start = start, .m_frames = frames};
}

bool open_source(const Take& take, size_t track, float gain, std::vector<Source>& sources) {
    auto decoder = std::make_unique<Decoder>();
    if (!decoder->open(take.track_path(track))) { return false; }
    // A take's files all have the take's rate: anything else is not this take's file.
    if (std::llround(take.m_sample_rate) != static_cast<long long>(decoder->sample_rate())) {
        return false;
    }
    const uint32_t channels = decoder->channels();
    if (channels == 0) { return false; }
    sources.push_back({.m_decoder = std::move(decoder), .m_channels = channels, .m_gain = gain});
    return true;
}

// Reads the sources block by block from `range` and hands every mixed block to `consume`
// (interleaved, `channels` channels). `progress` is called after each block with the part
// done, 0 to 1.
class BlockMixer {
public:
    BlockMixer(std::vector<Source>& sources, uint32_t channels)
        : m_sources(sources), m_channels(channels), m_mix(k_chunk_frames * channels) {
        uint32_t widest = 1;
        for (const Source& source : sources) { widest = std::max(widest, source.m_channels); }
        m_read.resize(k_chunk_frames * widest);
    }

    template <typename Consume, typename Progress>
    ExportResult run(const Range& range, Consume&& consume, Progress&& progress) {
        for (Source& source : m_sources) {
            if (range.m_frames > 0 && !source.m_decoder->seek(range.m_start)) {
                return ExportResult::Failed;
            }
        }
        uint64_t done = 0;
        while (done < range.m_frames) {
            const auto frames =
                static_cast<size_t>(std::min<uint64_t>(k_chunk_frames, range.m_frames - done));
            std::fill_n(m_mix.begin(), frames * m_channels, 0.0f);
            for (Source& source : m_sources) { add(source, frames); }
            if (!consume(m_mix.data(), frames)) { return ExportResult::Failed; }
            done += frames;
            const auto part =
                static_cast<float>(static_cast<double>(done) / static_cast<double>(range.m_frames));
            if (!progress(part)) { return ExportResult::Cancelled; }
        }
        return ExportResult::Done;
    }

private:
    void add(Source& source, size_t frames) {
        // A file shorter than the manifest says ends in silence.
        const size_t read = source.m_decoder->read(m_read.data(), frames);
        if (source.m_gain == 0.0f) { return; }
        const size_t in_channels = source.m_channels;
        for (size_t frame = 0; frame < read; ++frame) {
            const float* in = m_read.data() + (frame * in_channels);
            float* out = m_mix.data() + (frame * m_channels);
            if (in_channels == 1) {
                for (size_t channel = 0; channel < m_channels; ++channel) {
                    out[channel] += in[0] * source.m_gain;
                }
            } else {
                const size_t shared = std::min<size_t>(in_channels, m_channels);
                for (size_t channel = 0; channel < shared; ++channel) {
                    out[channel] += in[channel] * source.m_gain;
                }
            }
        }
    }

    std::vector<Source>& m_sources;
    uint32_t m_channels;
    std::vector<float> m_mix;
    std::vector<float> m_read;
};

// `scale` and `offset` map this export's own progress into the caller's 0 to 1.
ExportResult render(const Take& take,
                    std::vector<Source>& sources,
                    uint32_t channels,
                    const std::string& file_path,
                    const ExportOptions& options,
                    bool normalize,
                    float progress_offset = 0.0f,
                    float progress_scale = 1.0f) {
    const Range range = range_of(take, options);
    BlockMixer mixer(sources, channels);

    const auto report = [&](float part) {
        return !options.m_progress || options.m_progress(progress_offset + (part * progress_scale));
    };

    // Normalising needs the peak first: one pass to measure, one to write.
    float gain = 1.0f;
    const bool measure = normalize && options.m_normalize_db.has_value();
    if (measure) {
        float peak = 0.0f;
        const ExportResult measured = mixer.run(
            range,
            [&](const float* mix, size_t frames) {
                for (size_t i = 0; i < frames * channels; ++i) {
                    peak = std::max(peak, std::fabs(mix[i]));
                }
                return true;
            },
            [&](float part) { return report(part * 0.5f); });
        if (measured != ExportResult::Done) { return measured; }
        const float target = std::pow(10.0f, std::min(*options.m_normalize_db, 0.0f) / 20.0f);
        if (peak > 0.0f && std::isfinite(peak)) { gain = target / peak; }
    }

    WavStreamWriter writer;
    if (range.m_frames > WavStreamWriter::max_frames(channels, options.m_format) ||
        !writer.open(file_path,
                     {.m_channels = channels,
                      .m_sample_rate = take.m_sample_rate,
                      .m_format = options.m_format,
                      .m_dither = options.m_dither})) {
        return ExportResult::Failed;
    }

    std::vector<float> scaled(gain == 1.0f ? 0 : k_chunk_frames * channels);
    const ExportResult written = mixer.run(
        range,
        [&](const float* mix, size_t frames) {
            if (gain == 1.0f) { return writer.write(mix, frames); }
            for (size_t i = 0; i < frames * channels; ++i) { scaled[i] = mix[i] * gain; }
            return writer.write(scaled.data(), frames);
        },
        [&](float part) { return report(measure ? 0.5f + (part * 0.5f) : part); });
    if (written != ExportResult::Done) {
        writer.abort();
        return written;
    }
    return writer.close() ? ExportResult::Done : ExportResult::Failed;
}

}  // namespace

ExportResult export_mixdown(const Take& take,
                            const std::string& file_path,
                            const ExportOptions& options) {
    std::vector<Source> sources;
    uint32_t channels = 0;
    for (size_t track = 0; track < take.m_tracks.size(); ++track) {
        if (take.m_tracks[track].m_role != TrackRole::Stem) { continue; }
        const float gain =
            track < options.m_track_gains.size() ? options.m_track_gains[track] : 1.0f;
        if (!std::isfinite(gain)) { return ExportResult::Failed; }
        if (!open_source(take, track, gain, sources)) { return ExportResult::Failed; }
        channels = std::max(channels, sources.back().m_channels);
    }
    if (sources.empty()) { return ExportResult::Failed; }
    return render(take, sources, channels, file_path, options, true);
}

ExportResult export_track(const Take& take,
                          size_t track,
                          const std::string& file_path,
                          const ExportOptions& options) {
    if (track >= take.m_tracks.size()) { return ExportResult::Failed; }
    std::vector<Source> sources;
    if (!open_source(take, track, 1.0f, sources)) { return ExportResult::Failed; }
    return render(take, sources, sources.front().m_channels, file_path, options, true);
}

ExportResult export_tracks(const Take& take,
                           const std::string& directory,
                           const ExportOptions& options) {
    if (take.m_tracks.empty()) { return ExportResult::Failed; }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { return ExportResult::Failed; }
    // Into its own directory the export would replace the files it reads.
    if (std::filesystem::equivalent(directory, take.m_directory, error)) {
        return ExportResult::Failed;
    }

    // Every file is rendered under a temporary name first and only the complete set takes the
    // final names: a cancelled or failed export changes nothing that was in the directory.
    struct Pending {
        std::filesystem::path m_staged;
        std::filesystem::path m_final;
    };
    std::vector<Pending> pending;
    const float share = 1.0f / static_cast<float>(take.m_tracks.size());
    ExportResult result = ExportResult::Done;
    for (size_t track = 0; track < take.m_tracks.size() && result == ExportResult::Done; ++track) {
        std::vector<Source> sources;
        const std::filesystem::path base(directory);
        const std::filesystem::path staged =
            base / (".tanh-export-" + std::to_string(track) + ".partial");
        if (!open_source(take, track, 1.0f, sources)) {
            result = ExportResult::Failed;
            break;
        }
        result = render(take,
                        sources,
                        sources.front().m_channels,
                        staged.string(),
                        options,
                        false,
                        static_cast<float>(track) * share,
                        share);
        if (result == ExportResult::Done) {
            pending.push_back({.m_staged = staged, .m_final = base / take.m_tracks[track].m_file});
        }
    }

    for (const Pending& file : pending) {
        if (result == ExportResult::Done) {
            std::filesystem::rename(file.m_staged, file.m_final, error);
            if (error) { result = ExportResult::Failed; }
        }
        if (result != ExportResult::Done) { std::filesystem::remove(file.m_staged, error); }
    }
    return result;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
