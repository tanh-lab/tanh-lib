#include <tanh/audio-io/Take.h>
#include <tanh/audio-io/WavStreamWriter.h>
#include <tanh/core/AbiNamespace.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

namespace {

constexpr int k_manifest_version = 1;
constexpr const char* k_staging_name = ".take.json.partial";

const char* to_text(SampleFormat format) {
    switch (format) {
        case SampleFormat::Pcm16: return "pcm16";
        case SampleFormat::Pcm24: return "pcm24";
        case SampleFormat::Float32: return "float32";
    }
    return "float32";
}

std::optional<SampleFormat> format_from_text(const std::string& text) {
    if (text == "pcm16") { return SampleFormat::Pcm16; }
    if (text == "pcm24") { return SampleFormat::Pcm24; }
    if (text == "float32") { return SampleFormat::Float32; }
    return std::nullopt;
}

// JSON has no NaN and no infinity: a number that is neither would be written as null and
// make the manifest unreadable.
double finite(double value) {
    return std::isfinite(value) ? value : 0.0;
}

// The number at `key`, or `fallback` when it is missing or not a number of T's kind (an
// integer field holding a fraction or, where T is unsigned, something negative).
template <typename T>
T number_or(const nlohmann::json& object, const char* key, T fallback) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) { return fallback; }
    if constexpr (std::is_floating_point_v<T>) {
        return found->template get<T>();
    } else {
        if (!found->is_number_integer()) { return fallback; }
        if (std::is_unsigned_v<T> && !found->is_number_unsigned()) { return fallback; }
        return found->template get<T>();
    }
}

nlohmann::json to_json(const Take& take) {
    nlohmann::json tracks = nlohmann::json::array();
    for (const TakeTrack& track : take.m_tracks) {
        tracks.push_back({{"name", track.m_name},
                          {"file", track.m_file},
                          {"channels", track.m_channels},
                          {"role", track.m_role == TrackRole::Mix ? "mix" : "stem"}});
    }
    const TakeTiming& timing = take.m_timing;
    return {{"version", k_manifest_version},
            {"sample_rate", finite(take.m_sample_rate)},
            {"format", to_text(take.m_format)},
            {"num_frames", take.m_num_frames},
            {"dropped_frames", take.m_dropped_frames},
            {"truncated", take.m_truncated},
            {"timing",
             {{"bpm", finite(timing.m_bpm)},
              {"sig_num", timing.m_sig_num},
              {"sig_denom", timing.m_sig_denom},
              {"start_beat", finite(timing.m_start_beat)},
              {"length_beats", finite(timing.m_length_beats)},
              {"length_bars", finite(timing.length_bars())},
              {"length_frames", timing.m_length_frames}}},
            {"tracks", tracks}};
}

// Throws what nlohmann throws for a missing or mistyped field.
Take from_json(const nlohmann::json& json) {
    Take take;
    if (json.at("version").get<int>() > k_manifest_version) {
        throw std::runtime_error("manifest from a newer version");
    }
    take.m_sample_rate = json.at("sample_rate").get<double>();
    const std::optional<SampleFormat> format =
        format_from_text(json.at("format").get<std::string>());
    if (!format) { throw std::runtime_error("unknown sample format"); }
    take.m_format = *format;
    take.m_num_frames = json.at("num_frames").get<uint64_t>();
    take.m_dropped_frames = json.value("dropped_frames", uint64_t{0});
    take.m_truncated = json.value("truncated", false);

    if (const auto timing = json.find("timing"); timing != json.end()) {
        // The audio is what counts: timing that is missing or odd reads as "not known".
        if (timing->is_object()) {
            take.m_timing.m_bpm = number_or(*timing, "bpm", 0.0);
            take.m_timing.m_sig_num = number_or(*timing, "sig_num", int32_t{4});
            take.m_timing.m_sig_denom = number_or(*timing, "sig_denom", int32_t{4});
            take.m_timing.m_start_beat = number_or(*timing, "start_beat", 0.0);
            take.m_timing.m_length_beats = number_or(*timing, "length_beats", 0.0);
            take.m_timing.m_length_frames = number_or(*timing, "length_frames", uint64_t{0});
        }
    }

    for (const nlohmann::json& entry : json.at("tracks")) {
        TakeTrack track;
        track.m_name = entry.at("name").get<std::string>();
        track.m_file = entry.at("file").get<std::string>();
        track.m_channels = entry.at("channels").get<uint32_t>();
        track.m_role =
            entry.value("role", std::string("stem")) == "mix" ? TrackRole::Mix : TrackRole::Stem;
        // A manifest names files inside its own directory, nothing else.
        const std::filesystem::path file(track.m_file);
        if (track.m_file.empty() || track.m_channels == 0 || file.has_parent_path() ||
            file.is_absolute() || track.m_file == "." || track.m_file == "..") {
            throw std::runtime_error("unusable track entry");
        }
        take.m_tracks.push_back(std::move(track));
    }
    if (!(take.m_sample_rate >= 1.0)) { throw std::runtime_error("unusable sample rate"); }
    return take;
}

}  // namespace

std::string Take::track_path(size_t track) const {
    if (track >= m_tracks.size()) { return {}; }
    return (std::filesystem::path(m_directory) / m_tracks[track].m_file).string();
}

bool save_take(const Take& take) {
    const std::filesystem::path directory(take.m_directory);
    const std::filesystem::path staging = directory / k_staging_name;
    const std::filesystem::path destination = directory / Take::k_manifest_name;

    std::string text;
    try {
        // A name that is not valid UTF-8 is written with replacement characters instead
        // of costing the manifest.
        text = to_json(take).dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (const std::exception&) { return false; }
    text.push_back('\n');

    bool written = false;
    {
        std::ofstream file(staging, std::ios::binary | std::ios::trunc);
        if (file) {
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.flush();
            written = file.good();
        }
    }
    std::error_code error;
    if (written) { std::filesystem::rename(staging, destination, error); }
    if (!written || error) {
        std::filesystem::remove(staging, error);
        return false;
    }
    return true;
}

std::optional<Take> load_take(const std::string& directory) {
    const std::filesystem::path path = std::filesystem::path(directory) / Take::k_manifest_name;
    std::ifstream file(path, std::ios::binary);
    if (!file) { return std::nullopt; }
    try {
        Take take = from_json(nlohmann::json::parse(file));
        take.m_directory = directory;
        return take;
    } catch (const std::exception&) { return std::nullopt; }
}

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
