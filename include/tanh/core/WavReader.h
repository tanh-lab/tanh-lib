// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/core/Buffer.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace thl::core {

/**
 * Minimal, dependency-free RIFF/WAVE decoder.
 *
 * Decodes a WAV file (or an in-memory WAV image) into a planar BufferF with
 * the file's sample rate. Supported encodings:
 *  - PCM integer, 8 (unsigned), 16, 24 and 32 bit
 *  - IEEE float, 32 and 64 bit
 *  - the above wrapped in WAVE_FORMAT_EXTENSIBLE
 *
 * Integer samples are scaled to [-1, 1). Unknown chunks are skipped; the
 * first "fmt " and "data" chunks win. No resampling, no channel mixing.
 *
 * Failure semantics follow the other core containers: no logging, no
 * platform dependencies. On any error an empty buffer is returned and, if
 * @p error is non-null, a one-line reason is stored in it. Allocation
 * failure throws std::bad_alloc.
 *
 * This is deliberately tiny — for MP3/FLAC, streaming or resampling use
 * tanh::AudioIO's AudioFileLoader.
 */
namespace wav_detail {

inline uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline bool fail(std::string* error, const char* reason) {
    if (error != nullptr) { *error = reason; }
    return false;
}

}  // namespace wav_detail

/**
 * Decode a WAV image held in memory.
 *
 * @param data  Pointer to the first byte of the RIFF header.
 * @param size  Size of the image in bytes.
 * @param error Optional; receives a short reason when decoding fails.
 * @return Planar BufferF with the file's sample rate, or an empty buffer on
 *         failure.
 */
inline BufferF read_wav_from_memory(const void* data, size_t size, std::string* error = nullptr) {
    using namespace wav_detail;

    constexpr uint16_t k_format_pcm = 0x0001;
    constexpr uint16_t k_format_ieee_float = 0x0003;
    constexpr uint16_t k_format_extensible = 0xFFFE;

    const auto* bytes = static_cast<const uint8_t*>(data);
    if (bytes == nullptr || size < 12) {
        fail(error, "not a WAV file: too short");
        return {};
    }
    if (std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0) {
        fail(error, "not a WAV file: missing RIFF/WAVE header");
        return {};
    }

    uint16_t format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    bool fmt_read = false;
    const uint8_t* pcm = nullptr;
    size_t pcm_size = 0;

    size_t pos = 12;
    while (pos + 8 <= size && (!fmt_read || pcm == nullptr)) {
        const uint8_t* chunk_id = bytes + pos;
        const uint32_t chunk_size = read_u32(bytes + pos + 4);
        const size_t body = pos + 8;
        const size_t available = size - body;

        if (std::memcmp(chunk_id, "fmt ", 4) == 0 && !fmt_read) {
            if (chunk_size < 16 || chunk_size > available) {
                fail(error, "malformed fmt chunk");
                return {};
            }
            format = read_u16(bytes + body);
            num_channels = read_u16(bytes + body + 2);
            sample_rate = read_u32(bytes + body + 4);
            bits_per_sample = read_u16(bytes + body + 14);
            if (format == k_format_extensible) {
                // WAVE_FORMAT_EXTENSIBLE: the real format tag is the first two
                // bytes of the 16-byte SubFormat GUID at offset 24.
                if (chunk_size < 40) {
                    fail(error, "malformed WAVE_FORMAT_EXTENSIBLE fmt chunk");
                    return {};
                }
                format = read_u16(bytes + body + 24);
            }
            fmt_read = true;
        } else if (std::memcmp(chunk_id, "data", 4) == 0 && pcm == nullptr) {
            pcm = bytes + body;
            // Tolerate a truncated data chunk (streamed writers often leave a
            // placeholder size) by clamping to what is actually present.
            pcm_size = chunk_size > available ? available : chunk_size;
        }

        // Chunks are word-aligned: an odd-sized chunk is followed by a pad byte.
        const size_t padded = static_cast<size_t>(chunk_size) + (chunk_size & 1U);
        if (padded > available) { break; }
        pos = body + padded;
    }

    if (!fmt_read) {
        fail(error, "no fmt chunk");
        return {};
    }
    if (pcm == nullptr) {
        fail(error, "no data chunk");
        return {};
    }
    if (num_channels == 0 || sample_rate == 0) {
        fail(error, "fmt chunk has zero channels or sample rate");
        return {};
    }

    const bool is_float = format == k_format_ieee_float;
    const bool is_pcm = format == k_format_pcm;
    if (!(is_pcm && (bits_per_sample == 8 || bits_per_sample == 16 || bits_per_sample == 24 ||
                     bits_per_sample == 32)) &&
        !(is_float && (bits_per_sample == 32 || bits_per_sample == 64))) {
        fail(error, "unsupported sample format (need PCM 8/16/24/32 or float 32/64)");
        return {};
    }

    const size_t bytes_per_sample = bits_per_sample / 8U;
    const size_t frame_size = bytes_per_sample * num_channels;
    const size_t num_frames = pcm_size / frame_size;

    BufferF buffer(num_channels, num_frames, static_cast<double>(sample_rate));

    const uint8_t* src = pcm;
    for (size_t frame = 0; frame < num_frames; ++frame) {
        for (size_t ch = 0; ch < num_channels; ++ch, src += bytes_per_sample) {
            float value = 0.0F;
            if (is_float) {
                if (bits_per_sample == 32) {
                    uint32_t u = read_u32(src);
                    std::memcpy(&value, &u, sizeof(value));
                } else {
                    uint64_t u = static_cast<uint64_t>(read_u32(src)) |
                                 (static_cast<uint64_t>(read_u32(src + 4)) << 32);
                    double d = 0.0;
                    std::memcpy(&d, &u, sizeof(d));
                    value = static_cast<float>(d);
                }
            } else {
                switch (bits_per_sample) {
                    case 8: value = (static_cast<float>(src[0]) - 128.0F) / 128.0F; break;
                    case 16:
                        value = static_cast<float>(static_cast<int16_t>(read_u16(src))) / 32768.0F;
                        break;
                    case 24: {
                        int32_t s = static_cast<int32_t>((static_cast<uint32_t>(src[0]) << 8) |
                                                         (static_cast<uint32_t>(src[1]) << 16) |
                                                         (static_cast<uint32_t>(src[2]) << 24));
                        value = static_cast<float>(s >> 8) / 8388608.0F;
                        break;
                    }
                    default:  // 32
                        value =
                            static_cast<float>(static_cast<int32_t>(read_u32(src))) / 2147483648.0F;
                        break;
                }
            }
            buffer.get_write_pointer(ch)[frame] = value;
        }
    }
    return buffer;
}

/**
 * Decode a WAV file from disk.
 *
 * @param path  Path to the .wav file.
 * @param error Optional; receives a short reason when decoding fails.
 * @return Planar BufferF with the file's sample rate, or an empty buffer on
 *         failure (unreadable file or unsupported/malformed content).
 */
inline BufferF read_wav(const std::string& path, std::string* error = nullptr) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        wav_detail::fail(error, "cannot open file");
        return {};
    }
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    return read_wav_from_memory(image.data(), image.size(), error);
}

}  // namespace thl::core
