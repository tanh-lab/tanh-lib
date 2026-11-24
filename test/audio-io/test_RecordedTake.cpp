#include <gtest/gtest.h>
#include <tanh/audio-io/AudioFileLoader.h>
#include <tanh/audio-io/MultitrackRecorder.h>
#include <tanh/audio-io/Take.h>
#include <tanh/audio-io/TakeExport.h>
#include <tanh/core/Buffer.h>
#include <tanh/dsp/transport/RecordSchedule.h>
#include <tanh/dsp/transport/TransportInfo.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <system_error>

using namespace thl;
using namespace thl::audio_io;
using namespace thl::dsp::transport;

// The schedule decides, the recorder records: together a take is a whole number of bars on
// disk, with the tail after it and both lengths in the manifest.
TEST(RecordedTake, IsWholeBarsOnDiskWithItsTail) {
    constexpr double k_rate = 48000.0;
    constexpr uint32_t k_block = 256;
    constexpr uint64_t k_bar = 96000;  // 4/4 at 120 bpm
    constexpr uint64_t k_tail = 12000;

    std::random_device entropy;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("tanh_take_" + std::to_string(entropy()));

    MultitrackRecorder recorder;
    // This loop runs far faster than real time: room for the whole take, so that the disk
    // cannot fall behind it.
    ASSERT_TRUE(recorder.prepare(
        {.m_sample_rate = k_rate,
         .m_max_block_size = k_block,
         .m_buffer_seconds = 6.0,
         .m_tracks = {{.m_name = "engine", .m_channels = 2},
                      {.m_name = "mix", .m_channels = 2, .m_role = TrackRole::Mix}}}));
    RecordSchedule schedule;
    schedule.prepare(k_rate);
    schedule.set_tail_seconds(static_cast<double>(k_tail) / k_rate);

    // The sample counter of the "song" as the signal: the file shows where the take started.
    core::BufferF block(2, k_block);
    uint64_t song = 0;
    const double beats_per_sample = 120.0 / 60.0 / k_rate;
    const double first_beat = 2.25;  // armed in the middle of the first bar

    ASSERT_TRUE(recorder.begin_take(dir.string(), SampleFormat::Float32));
    schedule.arm(2);

    bool ended = false;
    for (int i = 0; i < 4000 && !ended; ++i) {
        TransportInfo transport;
        transport.m_flags = TransportInfo::k_has_tempo | TransportInfo::k_has_beat_position |
                            TransportInfo::k_has_time_signature | TransportInfo::k_is_playing;
        transport.m_num_samples = k_block;
        transport.m_bpm = 120.0;
        transport.m_beat_position = first_beat + (static_cast<double>(song) * beats_per_sample);
        transport.m_beats_per_sample = beats_per_sample;

        for (uint32_t s = 0; s < k_block; ++s) {
            const auto value = static_cast<float>((song + s) % 100000) * 1e-5f;
            block.get_write_pointer(0)[s] = value;
            block.get_write_pointer(1)[s] = -value;
        }

        const RecordWindow window = schedule.advance(transport);
        recorder.begin_block(
            k_block,
            {.m_begin = window.m_begin, .m_end = window.m_end, .m_final = window.m_final});
        recorder.write(0, block);
        recorder.write(1, block);
        if (window.m_final) {
            const RecordTiming& timing = schedule.timing();
            recorder.set_timing({.m_bpm = timing.m_bpm,
                                 .m_sig_num = timing.m_sig_num,
                                 .m_sig_denom = timing.m_sig_denom,
                                 .m_start_beat = timing.m_start_beat,
                                 .m_length_beats = timing.m_length_beats,
                                 .m_length_frames = timing.m_length_frames});
        }
        recorder.end_block();
        ended = window.m_final;
        song += k_block;
    }
    ASSERT_TRUE(ended);
    ASSERT_TRUE(recorder.wait_until_finished());
    ASSERT_EQ(recorder.state(), RecorderState::Done);

    const std::optional<Take> take = load_take(dir.string());
    ASSERT_TRUE(take.has_value());
    EXPECT_EQ(take->m_num_frames, (2 * k_bar) + k_tail);
    EXPECT_EQ(take->m_dropped_frames, 0U);
    EXPECT_EQ(take->musical_frames(), 2 * k_bar);
    EXPECT_DOUBLE_EQ(take->m_timing.length_bars(), 2.0);
    EXPECT_DOUBLE_EQ(take->m_timing.m_start_beat, 4.0);
    EXPECT_DOUBLE_EQ(take->m_timing.m_bpm, 120.0);

    // The first sample of the file is the sample of the bar line: beat 4 is 1.75 beats in.
    const uint64_t start = 42000;
    const core::BufferF file = AudioFileLoader().load_from_file(take->track_path(0));
    ASSERT_EQ(file.get_num_samples(), take->m_num_frames);
    for (const uint64_t frame : {uint64_t{0}, uint64_t{1}, k_bar, (2 * k_bar) + k_tail - 1}) {
        EXPECT_FLOAT_EQ(file.get_read_pointer(0)[frame],
                        static_cast<float>((start + frame) % 100000) * 1e-5f)
            << "frame " << frame;
    }

    // Exported without the tail, the mix is a file that loops.
    const std::string loop = (dir / "loop.wav").string();
    ExportOptions options;
    options.m_num_frames = take->musical_frames();
    ASSERT_EQ(export_track(*take, 1, loop, options), ExportResult::Done);
    EXPECT_EQ(AudioFileLoader().load_from_file(loop).get_num_samples(), 2 * k_bar);

    std::error_code error;
    std::filesystem::remove_all(dir, error);
}
