#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "tanh/core/WavReader.h"

using thl::core::BufferF;
using thl::core::read_wav;
using thl::core::read_wav_from_memory;

namespace {

void put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) { out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); }
}

void put_tag(std::vector<uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

// Build a minimal RIFF/WAVE image around raw little-endian PCM bytes.
std::vector<uint8_t> make_wav(uint16_t format,
                              uint16_t channels,
                              uint32_t sample_rate,
                              uint16_t bits,
                              const std::vector<uint8_t>& pcm,
                              bool extensible = false,
                              bool leading_junk_chunk = false) {
    std::vector<uint8_t> out;
    put_tag(out, "RIFF");
    put_u32(out, 0);  // patched below
    put_tag(out, "WAVE");

    if (leading_junk_chunk) {
        put_tag(out, "JUNK");
        put_u32(out, 3);  // odd size: exercises the pad byte
        out.insert(out.end(), {0, 0, 0, 0});
    }

    put_tag(out, "fmt ");
    put_u32(out, extensible ? 40 : 16);
    put_u16(out, extensible ? 0xFFFE : format);
    put_u16(out, channels);
    put_u32(out, sample_rate);
    put_u32(out, sample_rate * channels * bits / 8);
    put_u16(out, static_cast<uint16_t>(channels * bits / 8));
    put_u16(out, bits);
    if (extensible) {
        put_u16(out, 22);      // cbSize
        put_u16(out, bits);    // valid bits
        put_u32(out, 0);       // channel mask
        put_u16(out, format);  // SubFormat GUID starts with the format tag
        for (int i = 0; i < 14; ++i) { out.push_back(0); }
    }

    put_tag(out, "data");
    put_u32(out, static_cast<uint32_t>(pcm.size()));
    out.insert(out.end(), pcm.begin(), pcm.end());
    if (pcm.size() & 1U) { out.push_back(0); }

    const uint32_t riff_size = static_cast<uint32_t>(out.size() - 8);
    for (int i = 0; i < 4; ++i) {
        out[4 + i] = static_cast<uint8_t>((riff_size >> (8 * i)) & 0xFF);
    }
    return out;
}

}  // namespace

TEST(WavReader, Pcm16Stereo) {
    std::vector<uint8_t> pcm;
    const int16_t frames[3][2] = {{0, 16384}, {-32768, 32767}, {8192, -8192}};
    for (const auto& f : frames) {
        put_u16(pcm, static_cast<uint16_t>(f[0]));
        put_u16(pcm, static_cast<uint16_t>(f[1]));
    }
    const auto image = make_wav(1, 2, 48000, 16, pcm);

    std::string error;
    const BufferF buffer = read_wav_from_memory(image.data(), image.size(), &error);
    ASSERT_EQ(buffer.get_num_channels(), 2U) << error;
    ASSERT_EQ(buffer.get_num_samples(), 3U);
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 48000.0);

    const float* l = buffer.get_read_pointer(0);
    const float* r = buffer.get_read_pointer(1);
    EXPECT_FLOAT_EQ(l[0], 0.0F);
    EXPECT_FLOAT_EQ(r[0], 0.5F);
    EXPECT_FLOAT_EQ(l[1], -1.0F);
    EXPECT_FLOAT_EQ(r[1], 32767.0F / 32768.0F);
    EXPECT_FLOAT_EQ(l[2], 0.25F);
    EXPECT_FLOAT_EQ(r[2], -0.25F);
}

TEST(WavReader, Float32MonoExtensibleWithJunkChunk) {
    const std::vector<float> samples = {-0.5F, 0.0F, 0.75F, 1.0F, -1.0F};
    std::vector<uint8_t> pcm;
    for (float s : samples) {
        uint32_t u = 0;
        std::memcpy(&u, &s, sizeof(u));
        put_u32(pcm, u);
    }
    const auto image = make_wav(3, 1, 44100, 32, pcm, /*extensible=*/true, /*junk=*/true);

    const BufferF buffer = read_wav_from_memory(image.data(), image.size());
    ASSERT_EQ(buffer.get_num_channels(), 1U);
    ASSERT_EQ(buffer.get_num_samples(), samples.size());
    EXPECT_DOUBLE_EQ(buffer.get_sample_rate(), 44100.0);
    for (size_t i = 0; i < samples.size(); ++i) {
        EXPECT_FLOAT_EQ(buffer.get_read_pointer(0)[i], samples[i]);
    }
}

TEST(WavReader, Pcm8And24And32) {
    {
        const auto image = make_wav(1, 1, 8000, 8, {0, 128, 255});
        const BufferF b = read_wav_from_memory(image.data(), image.size());
        ASSERT_EQ(b.get_num_samples(), 3U);
        EXPECT_FLOAT_EQ(b.get_read_pointer(0)[0], -1.0F);
        EXPECT_FLOAT_EQ(b.get_read_pointer(0)[1], 0.0F);
        EXPECT_NEAR(b.get_read_pointer(0)[2], 1.0F, 1.0F / 128.0F);
    }
    {
        // 24-bit: 0x400000 = 0.5, 0x800000 = -1.0 (two's complement)
        const auto image = make_wav(1, 1, 8000, 24, {0x00, 0x00, 0x40, 0x00, 0x00, 0x80});
        const BufferF b = read_wav_from_memory(image.data(), image.size());
        ASSERT_EQ(b.get_num_samples(), 2U);
        EXPECT_FLOAT_EQ(b.get_read_pointer(0)[0], 0.5F);
        EXPECT_FLOAT_EQ(b.get_read_pointer(0)[1], -1.0F);
    }
    {
        std::vector<uint8_t> pcm;
        put_u32(pcm, 0x40000000U);
        put_u32(pcm, 0x80000000U);
        const auto image = make_wav(1, 1, 8000, 32, pcm);
        const BufferF b = read_wav_from_memory(image.data(), image.size());
        ASSERT_EQ(b.get_num_samples(), 2U);
        EXPECT_FLOAT_EQ(b.get_read_pointer(0)[0], 0.5F);
        EXPECT_FLOAT_EQ(b.get_read_pointer(0)[1], -1.0F);
    }
}

TEST(WavReader, Float64) {
    std::vector<uint8_t> pcm;
    const double d = 0.125;
    uint64_t u = 0;
    std::memcpy(&u, &d, sizeof(u));
    put_u32(pcm, static_cast<uint32_t>(u & 0xFFFFFFFFU));
    put_u32(pcm, static_cast<uint32_t>(u >> 32));
    const auto image = make_wav(3, 1, 8000, 64, pcm);
    const BufferF b = read_wav_from_memory(image.data(), image.size());
    ASSERT_EQ(b.get_num_samples(), 1U);
    EXPECT_FLOAT_EQ(b.get_read_pointer(0)[0], 0.125F);
}

TEST(WavReader, TruncatedDataChunkIsClamped) {
    std::vector<uint8_t> pcm;
    put_u16(pcm, 16384);
    put_u16(pcm, 16384);
    auto image = make_wav(1, 1, 8000, 16, pcm);
    // Claim more data than present (streaming writers leave 0xFFFFFFFF).
    image[image.size() - 4 - 4] = 0xFF;
    image[image.size() - 4 - 3] = 0xFF;
    image[image.size() - 4 - 2] = 0xFF;
    image[image.size() - 4 - 1] = 0xFF;
    const BufferF b = read_wav_from_memory(image.data(), image.size());
    EXPECT_EQ(b.get_num_samples(), 2U);
}

TEST(WavReader, RejectsGarbage) {
    std::string error;
    EXPECT_EQ(read_wav_from_memory(nullptr, 0, &error).get_num_channels(), 0U);
    EXPECT_FALSE(error.empty());

    const std::vector<uint8_t> not_riff = {'R', 'I', 'F', 'X', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    error.clear();
    EXPECT_EQ(read_wav_from_memory(not_riff.data(), not_riff.size(), &error).get_num_channels(),
              0U);
    EXPECT_FALSE(error.empty());

    // Unsupported: 12-bit PCM
    const auto odd_bits = make_wav(1, 1, 8000, 12, {0, 0});
    error.clear();
    EXPECT_EQ(read_wav_from_memory(odd_bits.data(), odd_bits.size(), &error).get_num_channels(),
              0U);
    EXPECT_NE(error.find("unsupported"), std::string::npos);

    // fmt but no data
    std::vector<uint8_t> no_data = make_wav(1, 1, 8000, 16, {});
    no_data.resize(no_data.size() - 8);  // drop the data chunk header
    error.clear();
    EXPECT_EQ(read_wav_from_memory(no_data.data(), no_data.size(), &error).get_num_channels(), 0U);
    EXPECT_NE(error.find("no data chunk"), std::string::npos);
}

TEST(WavReader, ReadsFromFile) {
    std::vector<uint8_t> pcm;
    put_u16(pcm, static_cast<uint16_t>(-16384));
    const auto image = make_wav(1, 1, 22050, 16, pcm);

    const std::string path = std::string(::testing::TempDir()) + "tanh_wavreader_test.wav";
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(image.data()),
                   static_cast<std::streamsize>(image.size()));
    }

    std::string error;
    const BufferF b = read_wav(path, &error);
    std::remove(path.c_str());
    ASSERT_EQ(b.get_num_samples(), 1U) << error;
    EXPECT_DOUBLE_EQ(b.get_sample_rate(), 22050.0);
    EXPECT_FLOAT_EQ(b.get_read_pointer(0)[0], -0.5F);

    error.clear();
    EXPECT_EQ(read_wav(path + ".missing", &error).get_num_channels(), 0U);
    EXPECT_EQ(error, "cannot open file");
}
