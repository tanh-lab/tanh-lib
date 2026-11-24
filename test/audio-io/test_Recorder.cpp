#include <gtest/gtest.h>
#include <tanh/audio-io/AudioFileLoader.h>
#include <tanh/audio-io/AudioFileSink.h>
#include <tanh/audio-io/MultitrackRecorder.h>
#include <tanh/audio-io/Take.h>
#include <tanh/audio-io/TakeExport.h>
#include <tanh/audio-io/WavStreamWriter.h>
#include <tanh/core/Buffer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/resource.h>
#endif

using namespace thl;
using namespace thl::audio_io;

namespace {

constexpr double k_rate = 48000.0;
constexpr uint32_t k_block = 128;
constexpr float k_poison = 9.0f;  // samples outside a span: must never reach a file

// A directory of its own per test, removed afterwards.
class TempDir {
public:
    TempDir() {
        std::random_device entropy;
        m_path = std::filesystem::temp_directory_path() /
                 ("tanh_recorder_" + std::to_string(entropy()) + "_" + std::to_string(entropy()));
        std::filesystem::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::filesystem::path operator/(const std::string& name) const {
        return m_path / name;
    }
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

// What track `track`, channel `channel` holds at frame `frame` of the take: never zero, so
// silence in a file is told apart from signal.
float signal(size_t track, size_t channel, uint64_t frame) {
    return (static_cast<float>((track * 4) + channel + 1) * 0.01f) +
           (static_cast<float>(frame % 4096) * 1e-4f);
}

core::BufferF load(const std::string& path) {
    return AudioFileLoader().load_from_file(path);
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

size_t count_entries(const std::filesystem::path& directory) {
    return static_cast<size_t>(std::distance(std::filesystem::directory_iterator(directory),
                                             std::filesystem::directory_iterator{}));
}

// Drives a recorder the way an audio callback does.
struct Rig {
    explicit Rig(std::vector<RecorderTrack> tracks, double buffer_seconds = 2.0) {
        RecorderConfig config;
        config.m_sample_rate = k_rate;
        config.m_max_block_size = k_block;
        config.m_buffer_seconds = buffer_seconds;
        config.m_tracks = std::move(tracks);
        EXPECT_TRUE(m_recorder.prepare(config));
        for (const RecorderTrack& track : config.m_tracks) {
            m_blocks.emplace_back(track.m_channels, k_block);
        }
    }

    // One block; `skip` is a track nobody writes.
    void block(RecordSpan span, std::optional<size_t> skip = std::nullopt) {
        for (size_t track = 0; track < m_blocks.size(); ++track) {
            for (size_t channel = 0; channel < m_blocks[track].get_num_channels(); ++channel) {
                float* samples = m_blocks[track].get_write_pointer(channel);
                for (uint32_t i = 0; i < k_block; ++i) {
                    const bool inside = i >= span.m_begin && i < span.m_end;
                    samples[i] =
                        inside ? signal(track, channel, m_frame + (i - span.m_begin)) : k_poison;
                }
            }
        }
        m_recorder.begin_block(k_block, span);
        for (size_t track = 0; track < m_blocks.size(); ++track) {
            if (skip && *skip == track) { continue; }
            m_recorder.write(track, m_blocks[track]);
        }
        m_recorder.end_block();
        m_frame += span.m_end - span.m_begin;
    }

    void whole_blocks(size_t count) {
        for (size_t i = 0; i < count; ++i) { block({.m_begin = 0, .m_end = k_block}); }
    }

    MultitrackRecorder m_recorder;
    std::vector<core::BufferF> m_blocks;
    uint64_t m_frame = 0;  // frames of the take handed over so far
};

void expect_signal(const core::BufferF& file, size_t track, uint64_t frames) {
    ASSERT_EQ(file.get_num_samples(), frames);
    for (size_t channel = 0; channel < file.get_num_channels(); ++channel) {
        const float* samples = file.get_read_pointer(channel);
        for (uint64_t frame = 0; frame < frames; ++frame) {
            ASSERT_FLOAT_EQ(samples[frame], signal(track, channel, frame))
                << "track " << track << " channel " << channel << " frame " << frame;
        }
    }
}

}  // namespace

// =============================================================================
// WavStreamWriter
// =============================================================================

TEST(WavStreamWriter, WritesBlockByBlockAndReadsBack) {
    const TempDir dir;
    const std::string path = (dir / "stream.wav").string();

    WavStreamWriter writer;
    ASSERT_TRUE(writer.open(path, {.m_channels = 2, .m_sample_rate = k_rate}));
    EXPECT_TRUE(writer.is_open());

    // Interleaved, planar, silence: every way in, one after the other.
    std::vector<float> interleaved(300 * 2);
    for (size_t i = 0; i < 300; ++i) {
        interleaved[i * 2] = signal(0, 0, i);
        interleaved[(i * 2) + 1] = signal(0, 1, i);
    }
    EXPECT_TRUE(writer.write(interleaved.data(), 300));

    core::BufferF planar(2, 5000);  // more than one internal block
    for (size_t i = 0; i < 5000; ++i) {
        planar.get_write_pointer(0)[i] = signal(0, 0, 300 + i);
        planar.get_write_pointer(1)[i] = signal(0, 1, 300 + i);
    }
    EXPECT_TRUE(writer.write(planar));
    EXPECT_TRUE(writer.write_silence(100));
    EXPECT_EQ(writer.frames_written(), 5400U);

    // Nothing at the destination until the file is complete.
    EXPECT_FALSE(std::filesystem::exists(path));
    ASSERT_TRUE(writer.close());
    EXPECT_FALSE(writer.is_open());
    EXPECT_EQ(count_entries(dir.path()), 1U) << "a staging file was left behind";

    const core::BufferF file = load(path);
    ASSERT_EQ(file.get_num_channels(), 2U);
    ASSERT_EQ(file.get_num_samples(), 5400U);
    EXPECT_DOUBLE_EQ(file.get_sample_rate(), k_rate);
    for (size_t channel = 0; channel < 2; ++channel) {
        for (size_t i = 0; i < 5300; ++i) {
            ASSERT_FLOAT_EQ(file.get_read_pointer(channel)[i], signal(0, channel, i));
        }
        for (size_t i = 5300; i < 5400; ++i) { ASSERT_EQ(file.get_read_pointer(channel)[i], 0.0f); }
    }
}

TEST(WavStreamWriter, Pcm24KeepsTheSignalAndClipsAtFullScale) {
    const TempDir dir;
    const std::string path = (dir / "pcm24.wav").string();

    const std::vector<float> samples = {0.0f, 0.25f, -0.5f, 0.999f, 1.5f, -1.5f, 1e-7f, 0.1f};
    WavStreamWriter writer;
    ASSERT_TRUE(
        writer.open(path,
                    {.m_channels = 1, .m_sample_rate = k_rate, .m_format = SampleFormat::Pcm24}));
    ASSERT_TRUE(writer.write(samples.data(), samples.size()));
    ASSERT_TRUE(writer.close());

    // 44 bytes of header, three bytes a sample.
    EXPECT_EQ(std::filesystem::file_size(path), 44U + (samples.size() * 3));

    const core::BufferF file = load(path);
    ASSERT_EQ(file.get_num_samples(), samples.size());
    const float step = 1.0f / 8388608.0f;
    for (size_t i = 0; i < samples.size(); ++i) {
        const float expected = std::clamp(samples[i], -1.0f, 1.0f - step);
        EXPECT_NEAR(file.get_read_pointer(0)[i], expected, step) << "sample " << i;
    }
}

TEST(WavStreamWriter, Pcm16DitherIsSmallAndReproducible) {
    const TempDir dir;
    std::vector<float> samples(2000);
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = 0.3f * std::sin(static_cast<float>(i) * 0.01f);
    }
    const auto write = [&](const std::string& name, bool dither) {
        const std::string path = (dir / name).string();
        WavStreamWriter writer;
        EXPECT_TRUE(writer.open(path,
                                {.m_channels = 1,
                                 .m_sample_rate = k_rate,
                                 .m_format = SampleFormat::Pcm16,
                                 .m_dither = dither}));
        EXPECT_TRUE(writer.write(samples.data(), samples.size()));
        EXPECT_TRUE(writer.close());
        return path;
    };

    const std::string plain = write("plain.wav", false);
    const std::string dithered = write("dithered.wav", true);
    const std::string again = write("again.wav", true);

    const float step = 1.0f / 32768.0f;
    const core::BufferF plain_file = load(plain);
    const core::BufferF dithered_file = load(dithered);
    ASSERT_EQ(plain_file.get_num_samples(), samples.size());
    ASSERT_EQ(dithered_file.get_num_samples(), samples.size());
    bool differs = false;
    for (size_t i = 0; i < samples.size(); ++i) {
        EXPECT_NEAR(plain_file.get_read_pointer(0)[i], samples[i], 0.5f * step + 1e-7f);
        EXPECT_NEAR(dithered_file.get_read_pointer(0)[i], samples[i], 1.5f * step + 1e-7f);
        differs =
            differs || plain_file.get_read_pointer(0)[i] != dithered_file.get_read_pointer(0)[i];
    }
    EXPECT_TRUE(differs) << "the dither changed nothing";
    EXPECT_EQ(read_bytes(dithered), read_bytes(again)) << "the same input gave another file";
}

TEST(WavStreamWriter, AbortAndFailureLeaveTheDestinationAsItWas) {
    const TempDir dir;
    const std::filesystem::path path = dir / "keep.wav";
    { std::ofstream(path, std::ios::binary) << "earlier"; }

    const std::vector<float> samples(64, 0.5f);
    {
        WavStreamWriter writer;
        ASSERT_TRUE(writer.open(path.string(), {.m_channels = 1, .m_sample_rate = k_rate}));
        ASSERT_TRUE(writer.write(samples.data(), samples.size()));
        writer.abort();
        EXPECT_FALSE(writer.is_open());
        EXPECT_FALSE(writer.close());
    }
    EXPECT_EQ(read_bytes(path), "earlier");
    EXPECT_EQ(count_entries(dir.path()), 1U);

    // A writer that goes away with its file open drops it too.
    {
        WavStreamWriter writer;
        ASSERT_TRUE(writer.open(path.string(), {.m_channels = 1, .m_sample_rate = k_rate}));
        ASSERT_TRUE(writer.write(samples.data(), samples.size()));
    }
    EXPECT_EQ(read_bytes(path), "earlier");
    EXPECT_EQ(count_entries(dir.path()), 1U);

    // A completed file replaces it.
    WavStreamWriter writer;
    ASSERT_TRUE(writer.open(path.string(), {.m_channels = 1, .m_sample_rate = k_rate}));
    ASSERT_TRUE(writer.write(samples.data(), samples.size()));
    ASSERT_TRUE(writer.close());
    EXPECT_EQ(load(path.string()).get_num_samples(), samples.size());
}

TEST(WavStreamWriter, RefusesWhatItCannotWrite) {
    const TempDir dir;
    const std::string path = (dir / "refused.wav").string();
    const std::vector<float> samples(16, 0.0f);

    WavStreamWriter writer;
    EXPECT_FALSE(writer.write(samples.data(), 8)) << "no file open";
    EXPECT_FALSE(writer.write_silence(8));
    EXPECT_FALSE(writer.close());
    EXPECT_EQ(writer.frames_left(), 0U);

    EXPECT_FALSE(writer.open(path, {.m_channels = 0, .m_sample_rate = k_rate}));
    EXPECT_FALSE(writer.open(path, {.m_channels = 2, .m_sample_rate = 0.0}));
    EXPECT_FALSE(writer.open(path, {.m_channels = 2, .m_sample_rate = 1.0e9}));
    EXPECT_FALSE(writer.open((dir / "missing" / "x.wav").string(),
                             {.m_channels = 2, .m_sample_rate = k_rate}));
    EXPECT_EQ(count_entries(dir.path()), 0U);

    ASSERT_TRUE(writer.open(path, {.m_channels = 2, .m_sample_rate = k_rate}));
    const core::BufferF mono(1, 8);
    EXPECT_FALSE(writer.write(mono)) << "one channel into a two-channel file";
    EXPECT_FALSE(writer.write(nullptr, 8));
    EXPECT_TRUE(writer.write(nullptr, 0));
    EXPECT_EQ(writer.frames_written(), 0U);
    writer.abort();
}

TEST(WavStreamWriter, KnowsHowMuchFits) {
    // 4 GiB minus the header, in frames.
    EXPECT_EQ(WavStreamWriter::max_frames(2, SampleFormat::Float32), (4294967295ULL - 37) / 8);
    EXPECT_EQ(WavStreamWriter::max_frames(1, SampleFormat::Pcm16), (4294967295ULL - 37) / 2);
    EXPECT_EQ(WavStreamWriter::max_frames(1, SampleFormat::Pcm24), (4294967295ULL - 37) / 3);
    EXPECT_EQ(WavStreamWriter::max_frames(0, SampleFormat::Pcm24), 0U);

    const TempDir dir;
    WavStreamWriter writer;
    ASSERT_TRUE(
        writer.open((dir / "left.wav").string(), {.m_channels = 2, .m_sample_rate = k_rate}));
    const uint64_t before = writer.frames_left();
    EXPECT_EQ(before, WavStreamWriter::max_frames(2, SampleFormat::Float32));
    ASSERT_TRUE(writer.write_silence(1000));
    EXPECT_EQ(writer.frames_left(), before - 1000);
    // More than fits: refused whole.
    EXPECT_FALSE(writer.write_silence(before));
    EXPECT_EQ(writer.frames_written(), 1000U);
    writer.abort();
}

TEST(WavStreamWriter, MovesWithItsOpenFile) {
    const TempDir dir;
    const std::string path = (dir / "moved.wav").string();
    const std::vector<float> samples(32, 0.25f);

    std::vector<WavStreamWriter> writers(1);
    ASSERT_TRUE(writers[0].open(path, {.m_channels = 1, .m_sample_rate = k_rate}));
    ASSERT_TRUE(writers[0].write(samples.data(), 16));
    writers.resize(8);  // moves the open writer
    WavStreamWriter moved = std::move(writers[0]);
    ASSERT_TRUE(moved.write(samples.data(), 16));
    ASSERT_TRUE(moved.close());
    EXPECT_EQ(load(path).get_num_samples(), 32U);
}

// =============================================================================
// MultitrackRecorder
// =============================================================================

TEST(MultitrackRecorder, TracksLineUpToTheSample) {
    const TempDir dir;
    Rig rig({{.m_name = "Engine 1", .m_channels = 2},
             {.m_name = "Return A", .m_channels = 1},
             {.m_name = "Mix", .m_channels = 2, .m_role = TrackRole::Mix}});

    std::atomic<int> callbacks{0};
    std::atomic<bool> callback_ok{false};
    rig.m_recorder.set_take_callback([&](const Take& take, bool ok) {
        callback_ok.store(ok && take.m_tracks.size() == 3);
        callbacks.fetch_add(1);
    });

    EXPECT_EQ(rig.m_recorder.state(), RecorderState::Idle);
    const std::string take_dir = (dir / "take-1").string();
    ASSERT_TRUE(rig.m_recorder.begin_take(take_dir));
    EXPECT_EQ(rig.m_recorder.state(), RecorderState::Ready);

    // Starts in the middle of a block, ends in the middle of another.
    rig.block({.m_begin = 0, .m_end = 0});
    rig.block({.m_begin = 40, .m_end = k_block});
    rig.whole_blocks(200);
    rig.m_recorder.set_timing({.m_bpm = 120.0,
                               .m_sig_num = 4,
                               .m_sig_denom = 4,
                               .m_start_beat = 8.0,
                               .m_length_beats = 16.0,
                               .m_length_frames = 20000});
    rig.block({.m_begin = 0, .m_end = 70, .m_final = true});
    // What comes after the end is not the take's.
    rig.whole_blocks(3);

    const uint64_t frames = (k_block - 40) + (200ULL * k_block) + 70;
    ASSERT_TRUE(rig.m_recorder.wait_until_finished());
    EXPECT_EQ(rig.m_recorder.state(), RecorderState::Done);
    EXPECT_EQ(rig.m_recorder.frames_recorded(), frames);
    EXPECT_EQ(rig.m_recorder.dropped_frames(), 0U);
    EXPECT_EQ(callbacks.load(), 1);
    EXPECT_TRUE(callback_ok.load());

    const std::optional<Take> take = rig.m_recorder.last_take();
    ASSERT_TRUE(take.has_value());
    EXPECT_EQ(take->m_num_frames, frames);
    EXPECT_EQ(take->m_directory, take_dir);
    EXPECT_FALSE(take->m_truncated);
    ASSERT_EQ(take->m_tracks.size(), 3U);
    EXPECT_EQ(take->m_tracks[0].m_file, "Engine 1.wav");
    EXPECT_EQ(take->m_tracks[2].m_role, TrackRole::Mix);
    EXPECT_DOUBLE_EQ(take->m_timing.m_length_beats, 16.0);
    EXPECT_DOUBLE_EQ(take->m_timing.length_bars(), 4.0);
    EXPECT_EQ(take->musical_frames(), 20000U);

    for (size_t track = 0; track < 3; ++track) {
        const core::BufferF file = load(take->track_path(track));
        EXPECT_EQ(file.get_num_channels(), take->m_tracks[track].m_channels);
        expect_signal(file, track, frames);
    }
    // Three files and the manifest, nothing else.
    EXPECT_EQ(count_entries(take_dir), 4U);

    // The manifest alone brings the take back.
    const std::optional<Take> loaded = load_take(take_dir);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->m_num_frames, frames);
    EXPECT_EQ(loaded->m_tracks[1].m_name, "Return A");
    EXPECT_EQ(loaded->m_tracks[1].m_channels, 1U);
    EXPECT_DOUBLE_EQ(loaded->m_timing.m_start_beat, 8.0);
}

TEST(MultitrackRecorder, TrackNobodyWroteIsSilentAndAsLong) {
    const TempDir dir;
    Rig rig({{.m_name = "a", .m_channels = 2}, {.m_name = "b", .m_channels = 2}});
    ASSERT_TRUE(rig.m_recorder.begin_take((dir / "take").string()));

    rig.whole_blocks(4);
    rig.block({.m_begin = 0, .m_end = k_block}, 1);  // track b left out
    rig.whole_blocks(4);
    rig.block({.m_begin = 0, .m_end = k_block, .m_final = true});
    ASSERT_TRUE(rig.m_recorder.wait_until_finished());

    const Take take = *rig.m_recorder.last_take();
    const uint64_t frames = 10ULL * k_block;
    expect_signal(load(take.track_path(0)), 0, frames);

    const core::BufferF b = load(take.track_path(1));
    ASSERT_EQ(b.get_num_samples(), frames);
    for (uint64_t frame = 0; frame < frames; ++frame) {
        const bool left_out = frame >= 4ULL * k_block && frame < 5ULL * k_block;
        ASSERT_FLOAT_EQ(b.get_read_pointer(0)[frame], left_out ? 0.0f : signal(1, 0, frame));
    }
}

TEST(MultitrackRecorder, MissingChannelsAreSilent) {
    const TempDir dir;
    MultitrackRecorder recorder;
    ASSERT_TRUE(recorder.prepare({.m_sample_rate = k_rate,
                                  .m_max_block_size = k_block,
                                  .m_tracks = {{.m_name = "stereo", .m_channels = 2}}}));
    ASSERT_TRUE(recorder.begin_take((dir / "take").string()));

    core::BufferF mono(1, k_block);
    std::fill_n(mono.get_write_pointer(0), k_block, 0.5f);
    recorder.begin_block(k_block, {.m_begin = 0, .m_end = k_block, .m_final = true});
    recorder.write(0, mono);
    recorder.write(0, mono);  // a second hand-over of the same track is ignored
    recorder.write(7, mono);  // so is a track that does not exist
    recorder.end_block();
    ASSERT_TRUE(recorder.wait_until_finished());

    const core::BufferF file = load(recorder.last_take()->track_path(0));
    ASSERT_EQ(file.get_num_samples(), k_block);
    EXPECT_FLOAT_EQ(file.get_read_pointer(0)[5], 0.5f);
    EXPECT_FLOAT_EQ(file.get_read_pointer(1)[5], 0.0f);
}

// A stop that is not on a grid, or a device that went away: the control thread ends it.
TEST(MultitrackRecorder, EndTakeKeepsWhatArrived) {
    const TempDir dir;
    Rig rig({{.m_name = "a", .m_channels = 1}});
    ASSERT_TRUE(rig.m_recorder.begin_take((dir / "take").string(), SampleFormat::Pcm24));
    rig.whole_blocks(30);
    rig.m_recorder.end_take({.m_bpm = 90.0});
    ASSERT_TRUE(rig.m_recorder.wait_until_finished());
    ASSERT_EQ(rig.m_recorder.state(), RecorderState::Done);

    // Blocks after the end go nowhere.
    rig.whole_blocks(5);

    const Take take = *rig.m_recorder.last_take();
    EXPECT_EQ(take.m_num_frames, 30ULL * k_block);
    EXPECT_EQ(take.m_format, SampleFormat::Pcm24);
    EXPECT_DOUBLE_EQ(take.m_timing.m_bpm, 90.0);
    EXPECT_FALSE(take.m_timing.is_musical());
    EXPECT_EQ(take.musical_frames(), take.m_num_frames);

    const core::BufferF file = load(take.track_path(0));
    ASSERT_EQ(file.get_num_samples(), take.m_num_frames);
    EXPECT_NEAR(file.get_read_pointer(0)[100], signal(0, 0, 100), 1.0f / 8388608.0f);
}

TEST(MultitrackRecorder, CancelRemovesTheTake) {
    const TempDir dir;
    Rig rig({{.m_name = "a", .m_channels = 2}});
    const std::filesystem::path take_dir = dir / "take";
    ASSERT_TRUE(rig.m_recorder.begin_take(take_dir.string()));
    rig.whole_blocks(20);
    rig.m_recorder.cancel_take();

    EXPECT_EQ(rig.m_recorder.state(), RecorderState::Idle);
    EXPECT_FALSE(std::filesystem::exists(take_dir));
    EXPECT_FALSE(rig.m_recorder.last_take().has_value());
    rig.whole_blocks(2);  // the audio thread goes on; nothing happens
}

// Whatever an abandoned take left in the buffers must not reach the next one.
TEST(MultitrackRecorder, TakeAfterACancelledOneStartsClean) {
    const TempDir dir;
    Rig rig({{.m_name = "a", .m_channels = 2}, {.m_name = "b", .m_channels = 1}});

    for (int round = 0; round < 5; ++round) {
        ASSERT_TRUE(rig.m_recorder.begin_take((dir / "dropped").string()));
        rig.whole_blocks(40);
        rig.m_recorder.cancel_take();
    }

    rig.m_frame = 0;
    ASSERT_TRUE(rig.m_recorder.begin_take((dir / "kept").string()));
    rig.whole_blocks(25);
    rig.block({.m_begin = 0, .m_end = k_block, .m_final = true});
    ASSERT_TRUE(rig.m_recorder.wait_until_finished());

    const Take take = *rig.m_recorder.last_take();
    expect_signal(load(take.track_path(0)), 0, 26ULL * k_block);
    expect_signal(load(take.track_path(1)), 1, 26ULL * k_block);
}

TEST(MultitrackRecorder, OneTakeAtATimeAndOneAfterTheOther) {
    const TempDir dir;
    Rig rig({{.m_name = "a", .m_channels = 1}});

    ASSERT_TRUE(rig.m_recorder.begin_take((dir / "one").string()));
    EXPECT_FALSE(rig.m_recorder.begin_take((dir / "two").string())) << "a take is open";
    rig.whole_blocks(3);
    rig.block({.m_begin = 0, .m_end = 10, .m_final = true});
    ASSERT_TRUE(rig.m_recorder.wait_until_finished());
    EXPECT_EQ(rig.m_recorder.last_take()->m_num_frames, (3ULL * k_block) + 10);

    rig.m_frame = 0;
    ASSERT_TRUE(rig.m_recorder.begin_take((dir / "two").string()));
    EXPECT_EQ(rig.m_recorder.frames_recorded(), 0U);
    rig.whole_blocks(7);
    rig.block({.m_begin = 0, .m_end = 0, .m_final = true});  // ends on the block boundary
    ASSERT_TRUE(rig.m_recorder.wait_until_finished());

    const Take take = *rig.m_recorder.last_take();
    EXPECT_EQ(take.m_directory, (dir / "two").string());
    expect_signal(load(take.track_path(0)), 0, 7ULL * k_block);
}

// The buffers hold two blocks and the audio thread does not wait: blocks get lost. Every file
// loses the same ones, as silence, so the take keeps its length and the tracks stay aligned.
TEST(MultitrackRecorder, SlowDiskCostsSilenceNotAlignment) {
    const TempDir dir;
    Rig rig({{.m_name = "a", .m_channels = 1}, {.m_name = "b", .m_channels = 1}}, 0.0);
    ASSERT_TRUE(rig.m_recorder.begin_take((dir / "take").string()));

    constexpr size_t k_blocks = 5000;
    rig.whole_blocks(k_blocks);
    rig.block({.m_begin = 0, .m_end = 0, .m_final = true});
    ASSERT_TRUE(rig.m_recorder.wait_until_finished());
    ASSERT_EQ(rig.m_recorder.state(), RecorderState::Done);

    const Take take = *rig.m_recorder.last_take();
    const uint64_t frames = static_cast<uint64_t>(k_blocks) * k_block;
    EXPECT_EQ(take.m_num_frames, frames);
    EXPECT_GT(take.m_dropped_frames, 0U);
    EXPECT_EQ(take.m_dropped_frames, rig.m_recorder.dropped_frames());

    const core::BufferF a = load(take.track_path(0));
    const core::BufferF b = load(take.track_path(1));
    ASSERT_EQ(a.get_num_samples(), frames);
    ASSERT_EQ(b.get_num_samples(), frames);
    uint64_t silent = 0;
    for (uint64_t frame = 0; frame < frames; ++frame) {
        const float in_a = a.get_read_pointer(0)[frame];
        const float in_b = b.get_read_pointer(0)[frame];
        if (in_a == 0.0f) {
            ++silent;
            ASSERT_EQ(in_b, 0.0f) << "the tracks lost different frames at " << frame;
        } else {
            ASSERT_FLOAT_EQ(in_a, signal(0, 0, frame)) << "frame " << frame << " is out of place";
            ASSERT_FLOAT_EQ(in_b, signal(1, 0, frame));
        }
    }
    EXPECT_EQ(silent, take.m_dropped_frames);
}

// Which tracks a take records is chosen per take: only the mix, only the stems, all of them.
// Nothing is prepared again in between, and the audio thread hands over every track always.
TEST(MultitrackRecorder, EachTakePicksItsTracks) {
    const TempDir dir;
    Rig rig({{.m_name = "engine", .m_channels = 2},
             {.m_name = "return", .m_channels = 1},
             {.m_name = "mix", .m_channels = 2, .m_role = TrackRole::Mix}});

    const auto record = [&](const std::string& name, const std::vector<size_t>& tracks) {
        rig.m_frame = 0;
        EXPECT_TRUE(
            rig.m_recorder.begin_take((dir / name).string(), SampleFormat::Float32, tracks));
        rig.whole_blocks(30);
        rig.block({.m_begin = 0, .m_end = 50, .m_final = true});
        EXPECT_TRUE(rig.m_recorder.wait_until_finished());
        EXPECT_EQ(rig.m_recorder.state(), RecorderState::Done);
        return *rig.m_recorder.last_take();
    };
    const uint64_t frames = (30ULL * k_block) + 50;

    // Only the mix.
    const Take mix_only = record("mix-only", {2});
    ASSERT_EQ(mix_only.m_tracks.size(), 1U);
    EXPECT_EQ(mix_only.m_tracks[0].m_name, "mix");
    EXPECT_EQ(mix_only.m_tracks[0].m_role, TrackRole::Mix);
    EXPECT_EQ(count_entries(dir / "mix-only"), 2U) << "one file and the manifest";
    expect_signal(load(mix_only.track_path(0)), 2, frames);

    // Only the stems, named in any order and twice: recorded once, in prepared order.
    const Take stems = record("stems", {1, 0, 1});
    ASSERT_EQ(stems.m_tracks.size(), 2U);
    EXPECT_EQ(stems.m_tracks[0].m_name, "engine");
    EXPECT_EQ(stems.m_tracks[1].m_name, "return");
    expect_signal(load(stems.track_path(0)), 0, frames);
    expect_signal(load(stems.track_path(1)), 1, frames);

    // A take that is dropped half way, with another choice again.
    ASSERT_TRUE(rig.m_recorder.begin_take((dir / "dropped").string(), SampleFormat::Float32, {0}));
    rig.whole_blocks(17);
    rig.m_recorder.cancel_take();

    // Everything: each buffer starts where this take starts, whatever the takes before left
    // in it.
    const Take all = record("all", {});
    ASSERT_EQ(all.m_tracks.size(), 3U);
    for (size_t track = 0; track < 3; ++track) {
        expect_signal(load(all.track_path(track)), track, frames);
    }

    // And the mix alone once more.
    const Take again = record("mix-again", {2});
    ASSERT_EQ(again.m_tracks.size(), 1U);
    expect_signal(load(again.track_path(0)), 2, frames);

    // A track that was not prepared cannot be picked.
    EXPECT_FALSE(rig.m_recorder.begin_take((dir / "bad").string(), SampleFormat::Float32, {0, 3}));
    EXPECT_FALSE(std::filesystem::exists(dir / "bad"));
    EXPECT_EQ(rig.m_recorder.state(), RecorderState::Done) << "the last take is untouched";
}

// A take that is dropped removes what it made, not what was there before it.
TEST(MultitrackRecorder, CancelKeepsWhatWasInTheDirectoryBefore) {
    const TempDir dir;
    Rig rig({{.m_name = "a", .m_channels = 2}, {.m_name = "b", .m_channels = 1}});
    const std::string take_dir = (dir / "current").string();

    ASSERT_TRUE(rig.m_recorder.begin_take(take_dir));
    rig.whole_blocks(10);
    rig.block({.m_begin = 0, .m_end = 0, .m_final = true});
    ASSERT_TRUE(rig.m_recorder.wait_until_finished());
    const Take first = *rig.m_recorder.last_take();
    { std::ofstream(dir / "current" / "notes.txt") << "keep me"; }

    // A second take into the same directory, dropped half way.
    rig.m_frame = 0;
    ASSERT_TRUE(rig.m_recorder.begin_take(take_dir));
    rig.whole_blocks(25);
    rig.m_recorder.cancel_take();
    EXPECT_EQ(rig.m_recorder.state(), RecorderState::Idle);

    EXPECT_EQ(count_entries(take_dir), 4U) << "two files, the manifest and the notes";
    const std::optional<Take> still = load_take(take_dir);
    ASSERT_TRUE(still.has_value());
    EXPECT_EQ(still->m_num_frames, first.m_num_frames);
    expect_signal(load(first.track_path(0)), 0, first.m_num_frames);
    expect_signal(load(first.track_path(1)), 1, first.m_num_frames);

    // A take that completes there replaces the files.
    rig.m_frame = 0;
    ASSERT_TRUE(rig.m_recorder.begin_take(take_dir));
    rig.whole_blocks(3);
    rig.block({.m_begin = 0, .m_end = 0, .m_final = true});
    ASSERT_TRUE(rig.m_recorder.wait_until_finished());
    EXPECT_EQ(load_take(take_dir)->m_num_frames, 3ULL * k_block);
    EXPECT_TRUE(std::filesystem::exists(dir / "current" / "notes.txt"));
}

// Cancel also drops a take whose end has been handed over and is still being written: after
// it, either nothing of the take is left, or the take was done before the cancel came.
TEST(MultitrackRecorder, CancelAfterTheEndStillDropsTheTake) {
    for (int round = 0; round < 20; ++round) {
        const TempDir dir;
        Rig rig({{.m_name = "a", .m_channels = 2}, {.m_name = "b", .m_channels = 2}});
        std::atomic<int> finished{0};
        rig.m_recorder.set_take_callback([&](const Take&, bool ok) {
            if (ok) { finished.fetch_add(1); }
        });
        const std::filesystem::path take_dir = dir / "take";

        ASSERT_TRUE(rig.m_recorder.begin_take(take_dir.string()));
        rig.whole_blocks(300);
        rig.block({.m_begin = 0, .m_end = 0, .m_final = true});
        rig.m_recorder.cancel_take();

        if (rig.m_recorder.state() == RecorderState::Idle) {
            EXPECT_FALSE(std::filesystem::exists(take_dir));
            EXPECT_EQ(finished.load(), 0);
            EXPECT_FALSE(rig.m_recorder.last_take().has_value());
        } else {
            ASSERT_EQ(rig.m_recorder.state(), RecorderState::Done);
            EXPECT_EQ(finished.load(), 1);
            EXPECT_TRUE(load_take(take_dir.string()).has_value());
        }
    }
}

// The audio thread may take as long as it likes over a block (an offline render, a debugger):
// the take waits for it.
TEST(MultitrackRecorder, SlowBlockDoesNotEndTheTake) {
    const TempDir dir;
    Rig rig({{.m_name = "a", .m_channels = 1}, {.m_name = "b", .m_channels = 1}});
    ASSERT_TRUE(rig.m_recorder.begin_take((dir / "take").string()));
    rig.whole_blocks(5);

    // One block with a long pause between its two tracks.
    for (size_t track = 0; track < 2; ++track) {
        float* samples = rig.m_blocks[track].get_write_pointer(0);
        for (uint32_t i = 0; i < k_block; ++i) { samples[i] = signal(track, 0, rig.m_frame + i); }
    }
    rig.m_recorder.begin_block(k_block);
    rig.m_recorder.write(0, rig.m_blocks[0]);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    rig.m_recorder.write(1, rig.m_blocks[1]);
    rig.m_recorder.end_block();
    rig.m_frame += k_block;

    rig.whole_blocks(5);
    rig.block({.m_begin = 0, .m_end = 0, .m_final = true});
    ASSERT_TRUE(rig.m_recorder.wait_until_finished());

    const Take take = *rig.m_recorder.last_take();
    EXPECT_FALSE(take.m_truncated);
    EXPECT_EQ(take.m_dropped_frames, 0U);
    expect_signal(load(take.track_path(0)), 0, 11ULL * k_block);
    expect_signal(load(take.track_path(1)), 1, 11ULL * k_block);
}

TEST(MultitrackRecorder, FileNamesAreSafeAndDistinct) {
    const TempDir dir;
    // "Bass" and "bass" are one file where the file system ignores case; a name that is not
    // valid UTF-8 must not cost the take its manifest.
    Rig rig({{.m_name = "Send/Return: A", .m_channels = 1},
             {.m_name = "Send/Return: A", .m_channels = 1},
             {.m_name = "", .m_channels = 1},
             {.m_name = "..", .m_channels = 1},
             {.m_name = "Bass", .m_channels = 1},
             {.m_name = "bass", .m_channels = 1},
             {.m_name = "Ger\xE4t", .m_channels = 1}});
    ASSERT_TRUE(rig.m_recorder.begin_take((dir / "take").string()));
    rig.block({.m_begin = 0, .m_end = k_block, .m_final = true});
    ASSERT_TRUE(rig.m_recorder.wait_until_finished());

    const Take take = *rig.m_recorder.last_take();
    EXPECT_EQ(take.m_tracks[0].m_file, "Send_Return_ A.wav");
    EXPECT_EQ(take.m_tracks[1].m_file, "Send_Return_ A-2.wav");
    EXPECT_EQ(take.m_tracks[2].m_file, "track.wav");
    EXPECT_EQ(take.m_tracks[3].m_file, "track-2.wav");
    EXPECT_EQ(take.m_tracks[4].m_file, "Bass.wav");
    EXPECT_EQ(take.m_tracks[5].m_file, "bass-2.wav");
    EXPECT_EQ(take.m_tracks[6].m_file, "Ger_t.wav");
    EXPECT_EQ(take.m_tracks[0].m_name, "Send/Return: A") << "the name itself is kept";
    for (size_t track = 0; track < 7; ++track) {
        expect_signal(load(take.track_path(track)), track, k_block);
    }
    EXPECT_EQ(count_entries(take.m_directory), 8U);

    const std::optional<Take> loaded = load_take(take.m_directory);
    ASSERT_TRUE(loaded.has_value()) << "the manifest is there and readable";
    ASSERT_EQ(loaded->m_tracks.size(), 7U);
    EXPECT_EQ(loaded->m_tracks[6].m_file, "Ger_t.wav");
}

TEST(MultitrackRecorder, RefusesWhatItCannotRecord) {
    const TempDir dir;
    MultitrackRecorder recorder;
    EXPECT_FALSE(recorder.begin_take((dir / "take").string())) << "not prepared";
    EXPECT_FALSE(recorder.prepare({.m_sample_rate = k_rate, .m_tracks = {}}));
    EXPECT_FALSE(recorder.prepare(
        {.m_sample_rate = k_rate, .m_tracks = {{.m_name = "a", .m_channels = 0}}}));
    EXPECT_FALSE(
        recorder.prepare({.m_sample_rate = 0.0, .m_tracks = {{.m_name = "a", .m_channels = 1}}}));

    // Calls from the audio thread without a take, or before prepare(), do nothing.
    const core::BufferF block(1, k_block);
    recorder.begin_block(k_block);
    recorder.write(0, block);
    recorder.end_block();

    ASSERT_TRUE(recorder.prepare(
        {.m_sample_rate = k_rate, .m_tracks = {{.m_name = "a", .m_channels = 1}}}));
    recorder.begin_block(k_block);
    recorder.write(0, block);
    recorder.end_block();
    recorder.end_take();
    recorder.cancel_take();
    EXPECT_EQ(recorder.state(), RecorderState::Idle);

    // A directory that cannot be made: a file is in the way.
    const std::filesystem::path blocked = dir / "blocked";
    { std::ofstream(blocked) << "x"; }
    EXPECT_FALSE(recorder.begin_take((blocked / "take").string()));
    EXPECT_EQ(recorder.state(), RecorderState::Idle);
}

// =============================================================================
// Take manifest
// =============================================================================

TEST(Take, ManifestRoundTrip) {
    const TempDir dir;
    Take take;
    take.m_directory = dir.path().string();
    take.m_sample_rate = 44100.0;
    take.m_format = SampleFormat::Pcm16;
    take.m_num_frames = 123456;
    take.m_dropped_frames = 7;
    take.m_truncated = true;
    take.m_timing = {.m_bpm = 97.5,
                     .m_sig_num = 6,
                     .m_sig_denom = 8,
                     .m_start_beat = 12.0,
                     .m_length_beats = 6.0,
                     .m_length_frames = 100000};
    take.m_tracks = {
        {.m_name = "Ünïcode ♪", .m_file = "a.wav", .m_channels = 2},
        {.m_name = "Mix", .m_file = "mix.wav", .m_channels = 2, .m_role = TrackRole::Mix}};
    ASSERT_TRUE(save_take(take));
    EXPECT_EQ(count_entries(dir.path()), 1U);

    const std::optional<Take> loaded = load_take(dir.path().string());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_DOUBLE_EQ(loaded->m_sample_rate, 44100.0);
    EXPECT_EQ(loaded->m_format, SampleFormat::Pcm16);
    EXPECT_EQ(loaded->m_num_frames, 123456U);
    EXPECT_EQ(loaded->m_dropped_frames, 7U);
    EXPECT_TRUE(loaded->m_truncated);
    EXPECT_DOUBLE_EQ(loaded->m_timing.m_bpm, 97.5);
    EXPECT_EQ(loaded->m_timing.m_sig_num, 6);
    EXPECT_EQ(loaded->m_timing.m_sig_denom, 8);
    EXPECT_DOUBLE_EQ(loaded->m_timing.m_length_beats, 6.0);
    EXPECT_DOUBLE_EQ(loaded->m_timing.length_bars(), 2.0);  // 6/8: three quarter notes a bar
    EXPECT_EQ(loaded->musical_frames(), 100000U);
    ASSERT_EQ(loaded->m_tracks.size(), 2U);
    EXPECT_EQ(loaded->m_tracks[0].m_name, "Ünïcode ♪");
    EXPECT_EQ(loaded->m_tracks[1].m_role, TrackRole::Mix);
    EXPECT_EQ(loaded->track_path(1), (dir / "mix.wav").string());
    EXPECT_TRUE(loaded->track_path(2).empty());
}

// Timing nobody can count with must not make the take unreadable: it reads as "not known".
TEST(Take, OddTimingDoesNotCostTheManifest) {
    const TempDir dir;
    Take take;
    take.m_directory = dir.path().string();
    take.m_sample_rate = 48000.0;
    take.m_num_frames = 10;
    take.m_timing.m_bpm = std::numeric_limits<double>::quiet_NaN();
    take.m_timing.m_length_beats = std::numeric_limits<double>::infinity();
    take.m_tracks = {{.m_name = "a", .m_file = "a.wav", .m_channels = 1}};
    ASSERT_TRUE(save_take(take));

    std::optional<Take> loaded = load_take(dir.path().string());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_DOUBLE_EQ(loaded->m_timing.m_bpm, 0.0);
    EXPECT_FALSE(loaded->m_timing.is_musical());
    EXPECT_EQ(loaded->musical_frames(), 10U);

    // Written by something else, with nulls and the wrong kinds of value.
    std::ofstream(dir / Take::k_manifest_name, std::ios::binary | std::ios::trunc)
        << R"({"version": 1, "sample_rate": 48000, "format": "pcm24", "num_frames": 10,
               "timing": {"bpm": null, "sig_num": "four", "length_beats": 8.0,
                          "length_frames": -3},
               "tracks": [{"name": "a", "file": "a.wav", "channels": 1}]})";
    loaded = load_take(dir.path().string());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_DOUBLE_EQ(loaded->m_timing.m_bpm, 0.0);
    EXPECT_EQ(loaded->m_timing.m_sig_num, 4);
    EXPECT_DOUBLE_EQ(loaded->m_timing.m_length_beats, 8.0);
    EXPECT_EQ(loaded->m_timing.m_length_frames, 0U);
}

TEST(Take, LoadRejectsWhatIsNotATake) {
    const TempDir dir;
    EXPECT_FALSE(load_take(dir.path().string()).has_value()) << "no manifest";
    EXPECT_FALSE(load_take((dir / "missing").string()).has_value());

    const auto write = [&](const std::string& text) {
        std::ofstream(dir / Take::k_manifest_name, std::ios::binary | std::ios::trunc) << text;
    };
    write("not json");
    EXPECT_FALSE(load_take(dir.path().string()).has_value());
    write(R"({"version": 1})");
    EXPECT_FALSE(load_take(dir.path().string()).has_value());
    write(R"({"version": 99, "sample_rate": 48000, "format": "float32", "num_frames": 1,
              "tracks": []})");
    EXPECT_FALSE(load_take(dir.path().string()).has_value()) << "a newer manifest";

    // A manifest must not point outside its directory.
    for (const std::string file : {"../other.wav", "/etc/passwd", "sub/a.wav", "..", ""}) {
        write(R"({"version": 1, "sample_rate": 48000, "format": "float32", "num_frames": 1,
                  "tracks": [{"name": "a", "file": ")" +
              file + R"(", "channels": 2}]})");
        EXPECT_FALSE(load_take(dir.path().string()).has_value()) << "file: " << file;
    }

    write(R"({"version": 1, "sample_rate": 48000, "format": "float32", "num_frames": 1,
              "tracks": [{"name": "a", "file": "a.wav", "channels": 2}]})");
    EXPECT_TRUE(load_take(dir.path().string()).has_value());
}

// =============================================================================
// Export
// =============================================================================

namespace {

// A take on disk: two stereo stems, a mono stem and the mix, each holding signal().
Take record_take(const TempDir& dir, size_t blocks, uint64_t musical_frames = 0) {
    Rig rig({{.m_name = "one", .m_channels = 2},
             {.m_name = "two", .m_channels = 2},
             {.m_name = "mono", .m_channels = 1},
             {.m_name = "mix", .m_channels = 2, .m_role = TrackRole::Mix}});
    EXPECT_TRUE(rig.m_recorder.begin_take((dir / "take").string()));
    rig.whole_blocks(blocks);
    rig.m_recorder.set_timing({.m_bpm = 120.0,
                               .m_length_beats = musical_frames > 0 ? 4.0 : 0.0,
                               .m_length_frames = musical_frames});
    rig.block({.m_begin = 0, .m_end = 0, .m_final = true});
    EXPECT_TRUE(rig.m_recorder.wait_until_finished());
    return *rig.m_recorder.last_take();
}

}  // namespace

TEST(TakeExport, MixdownSumsTheStemsAndLeavesTheMixOut) {
    const TempDir dir;
    const Take take = record_take(dir, 40);
    const std::string path = (dir / "mixdown.wav").string();

    std::vector<float> progress;
    ExportOptions options;
    options.m_format = SampleFormat::Float32;
    options.m_track_gains = {1.0f, 0.5f};  // the mono stem and the mix keep 1
    options.m_progress = [&](float part) {
        progress.push_back(part);
        return true;
    };
    ASSERT_EQ(export_mixdown(take, path, options), ExportResult::Done);

    const core::BufferF file = load(path);
    ASSERT_EQ(file.get_num_channels(), 2U);
    ASSERT_EQ(file.get_num_samples(), take.m_num_frames);
    for (size_t channel = 0; channel < 2; ++channel) {
        for (uint64_t frame = 0; frame < take.m_num_frames; ++frame) {
            const float expected = signal(0, channel, frame) + (0.5f * signal(1, channel, frame)) +
                                   signal(2, 0, frame);  // the mono stem on both sides
            ASSERT_NEAR(file.get_read_pointer(channel)[frame], expected, 1e-6f);
        }
    }

    ASSERT_FALSE(progress.empty());
    EXPECT_TRUE(std::ranges::is_sorted(progress));
    EXPECT_FLOAT_EQ(progress.back(), 1.0f);

    // A muted stem is not in it.
    options.m_track_gains = {0.0f, 0.0f, 1.0f};
    options.m_progress = nullptr;
    ASSERT_EQ(export_mixdown(take, path, options), ExportResult::Done);
    const core::BufferF muted = load(path);
    EXPECT_NEAR(muted.get_read_pointer(1)[77], signal(2, 0, 77), 1e-6f);
}

TEST(TakeExport, RangeAndTheLengthWithoutTheTail) {
    const TempDir dir;
    const Take take = record_take(dir, 40, 3000);
    ASSERT_EQ(take.musical_frames(), 3000U);
    const std::string path = (dir / "loop.wav").string();

    ExportOptions options;
    options.m_format = SampleFormat::Float32;
    options.m_num_frames = take.musical_frames();
    ASSERT_EQ(export_track(take, 0, path, options), ExportResult::Done);
    expect_signal(load(path), 0, 3000);

    options.m_start_frame = 1000;
    options.m_num_frames = 500;
    ASSERT_EQ(export_track(take, 0, path, options), ExportResult::Done);
    const core::BufferF part = load(path);
    ASSERT_EQ(part.get_num_samples(), 500U);
    EXPECT_FLOAT_EQ(part.get_read_pointer(1)[0], signal(0, 1, 1000));
    EXPECT_FLOAT_EQ(part.get_read_pointer(1)[499], signal(0, 1, 1499));

    // More than there is: up to the end.
    options.m_start_frame = take.m_num_frames - 10;
    options.m_num_frames = 1000;
    ASSERT_EQ(export_track(take, 0, path, options), ExportResult::Done);
    EXPECT_EQ(load(path).get_num_samples(), 10U);
}

TEST(TakeExport, NormalizePutsThePeakWhereAsked) {
    const TempDir dir;
    const Take take = record_take(dir, 40);
    const std::string path = (dir / "normalized.wav").string();

    ExportOptions options;
    options.m_format = SampleFormat::Float32;
    options.m_normalize_db = -6.0f;
    ASSERT_EQ(export_mixdown(take, path, options), ExportResult::Done);

    const core::BufferF file = load(path);
    float peak = 0.0f;
    for (size_t channel = 0; channel < file.get_num_channels(); ++channel) {
        for (size_t frame = 0; frame < file.get_num_samples(); ++frame) {
            peak = std::max(peak, std::fabs(file.get_read_pointer(channel)[frame]));
        }
    }
    EXPECT_NEAR(peak, std::pow(10.0f, -6.0f / 20.0f), 1e-5f);
}

TEST(TakeExport, EveryTrackWithTheSameRange) {
    const TempDir dir;
    const Take take = record_take(dir, 40);
    const std::filesystem::path out = dir / "stems" / "nested";

    ExportOptions options;
    options.m_format = SampleFormat::Pcm24;
    options.m_num_frames = 2000;
    options.m_normalize_db = 0.0f;  // ignored: the files keep their balance
    ASSERT_EQ(export_tracks(take, out.string(), options), ExportResult::Done);

    EXPECT_EQ(count_entries(out), 4U);
    for (size_t track = 0; track < take.m_tracks.size(); ++track) {
        const core::BufferF file = load((out / take.m_tracks[track].m_file).string());
        ASSERT_EQ(file.get_num_samples(), 2000U);
        EXPECT_EQ(file.get_num_channels(), take.m_tracks[track].m_channels);
        EXPECT_NEAR(file.get_read_pointer(0)[1234], signal(track, 0, 1234), 1.0f / 8388608.0f);
    }
}

// A stems export that does not complete changes nothing in its directory, also not files
// from an earlier export; and it never writes into the take it reads.
TEST(TakeExport, StemsExportIsAllOrNothing) {
    const TempDir dir;
    const Take take = record_take(dir, 200);
    const std::filesystem::path out = dir / "stems";

    ExportOptions options;
    options.m_format = SampleFormat::Float32;
    options.m_num_frames = 1000;
    ASSERT_EQ(export_tracks(take, out.string(), options), ExportResult::Done);
    ASSERT_EQ(count_entries(out), 4U);

    // Again with another range, cancelled in the third file: the first export is untouched.
    options.m_num_frames = 0;
    options.m_progress = [](float part) { return part < 0.6f; };
    EXPECT_EQ(export_tracks(take, out.string(), options), ExportResult::Cancelled);
    EXPECT_EQ(count_entries(out), 4U);
    for (size_t track = 0; track < take.m_tracks.size(); ++track) {
        expect_signal(load((out / take.m_tracks[track].m_file).string()), track, 1000);
    }

    // Completed, it replaces them.
    options.m_progress = nullptr;
    ASSERT_EQ(export_tracks(take, out.string(), options), ExportResult::Done);
    EXPECT_EQ(count_entries(out), 4U);
    expect_signal(load((out / take.m_tracks[0].m_file).string()), 0, take.m_num_frames);

    // Into the take's own directory: refused, the take stays as it is.
    options.m_num_frames = 10;
    EXPECT_EQ(export_tracks(take, take.m_directory, options), ExportResult::Failed);
    EXPECT_EQ(
        export_tracks(take, (std::filesystem::path(take.m_directory) / ".").string(), options),
        ExportResult::Failed);
    EXPECT_EQ(count_entries(take.m_directory), 5U);
    expect_signal(load(take.track_path(0)), 0, take.m_num_frames);
}

TEST(TakeExport, CancelAndFailureLeaveNothingBehind) {
    const TempDir dir;
    const Take take = record_take(dir, 200);
    const std::filesystem::path out = dir / "out";
    std::filesystem::create_directories(out);

    ExportOptions options;
    int calls = 0;
    options.m_progress = [&](float) { return ++calls < 3; };
    EXPECT_EQ(export_mixdown(take, (out / "mix.wav").string(), options), ExportResult::Cancelled);
    EXPECT_EQ(count_entries(out), 0U);

    calls = 0;
    options.m_normalize_db = -1.0f;  // cancelled while measuring
    EXPECT_EQ(export_track(take, 3, (out / "mix.wav").string(), options), ExportResult::Cancelled);
    EXPECT_EQ(count_entries(out), 0U);

    // Cancelled in the second file: the first goes too.
    calls = 0;
    options.m_normalize_db.reset();
    options.m_progress = [&](float part) { return part < 0.3f; };
    EXPECT_EQ(export_tracks(take, out.string(), options), ExportResult::Cancelled);
    EXPECT_EQ(count_entries(out), 0U);

    options.m_progress = nullptr;
    EXPECT_EQ(export_track(take, 9, (out / "x.wav").string(), options), ExportResult::Failed);
    EXPECT_EQ(export_mixdown(take, (dir / "missing" / "x.wav").string(), options),
              ExportResult::Failed);

    // A stem that is gone.
    std::filesystem::remove(take.track_path(1));
    EXPECT_EQ(export_mixdown(take, (out / "mix.wav").string(), options), ExportResult::Failed);
    EXPECT_EQ(export_tracks(take, out.string(), options), ExportResult::Failed);
    EXPECT_EQ(count_entries(out), 0U);

    // A take without stems has nothing to mix.
    Take only_mix = take;
    only_mix.m_tracks = {take.m_tracks[3]};
    EXPECT_EQ(export_mixdown(only_mix, (out / "mix.wav").string(), options), ExportResult::Failed);
}

// =============================================================================
// AudioFileSink: the audio thread hands over, a thread of the sink writes
// =============================================================================

TEST(AudioFileSink, EverythingAcceptedIsInTheFileAfterClose) {
    const TempDir dir;
    const std::string path = (dir / "sink.wav").string();

    AudioFileSink sink;
    ASSERT_TRUE(sink.open_file(path, 2, 48000));
    sink.start_recording();

    constexpr uint32_t k_frames = 256;
    constexpr size_t k_blocks = 400;
    std::vector<float> input(k_frames * 2);
    for (size_t block = 0; block < k_blocks; ++block) {
        for (size_t i = 0; i < k_frames; ++i) {
            input[i * 2] = signal(0, 0, (block * k_frames) + i);
            input[(i * 2) + 1] = signal(0, 1, (block * k_frames) + i);
        }
        sink.process(nullptr, input.data(), k_frames, 2, 0);
    }
    // Whatever the sink had no room for is counted, never written as half a block.
    const uint64_t accepted = sink.get_frames_written();
    EXPECT_EQ(accepted + sink.get_frames_dropped(), k_blocks * k_frames);

    sink.close_file();
    const core::BufferF file = load(path);
    ASSERT_EQ(file.get_num_samples(), accepted);
    if (sink.get_frames_dropped() == 0) { expect_signal(file, 0, accepted); }
}

TEST(AudioFileSink, CloseSaysWhetherTheFileIsThere) {
    const TempDir dir;
    const std::string path = (dir / "sink.wav").string();
    const std::vector<float> input(512, 0.25f);

    AudioFileSink sink;
    EXPECT_FALSE(sink.close_file()) << "nothing open";
    ASSERT_TRUE(sink.open_file(path, 1, 48000));
    sink.start_recording();
    sink.process(nullptr, input.data(), 512, 1, 0);
    EXPECT_TRUE(sink.close_file());
    EXPECT_FALSE(sink.has_failed());
    EXPECT_EQ(load(path).get_num_samples(), 512U);
    EXPECT_FALSE(sink.close_file()) << "already closed";
}

#ifndef _WIN32
// A disk that takes no more: the caller hears about it instead of finding no file later.
TEST(AudioFileSink, ReportsAFileItCouldNotWrite) {
    const TempDir dir;
    const std::string path = (dir / "full.wav").string();
    const std::vector<float> input(4096, 0.25f);

    rlimit original_limit{};
    ASSERT_EQ(getrlimit(RLIMIT_FSIZE, &original_limit), 0);
    rlimit tiny_limit = original_limit;
    tiny_limit.rlim_cur = 1024;
    const auto previous_handler = std::signal(SIGXFSZ, SIG_IGN);

    AudioFileSink sink;
    ASSERT_TRUE(sink.open_file(path, 1, 48000));
    sink.start_recording();
    ASSERT_EQ(setrlimit(RLIMIT_FSIZE, &tiny_limit), 0);
    for (int i = 0; i < 64; ++i) { sink.process(nullptr, input.data(), 4096, 1, 0); }
    const bool closed = sink.close_file();
    setrlimit(RLIMIT_FSIZE, &original_limit);
    std::signal(SIGXFSZ, previous_handler);

    EXPECT_FALSE(closed);
    EXPECT_TRUE(sink.has_failed());
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(count_entries(dir.path()), 0U);
}
#endif

TEST(AudioFileSink, RefusesAFormatItCannotWrite) {
    const TempDir dir;
    AudioFileSink sink;
    EXPECT_FALSE(sink.open_file((dir / "sink.flac").string(), 2, 48000, AudioEncodingFormat::FLAC));
    EXPECT_FALSE(sink.is_open());
    EXPECT_FALSE(sink.open_file((dir / "missing" / "sink.wav").string(), 2, 48000));
    EXPECT_EQ(count_entries(dir.path()), 0U);
}
