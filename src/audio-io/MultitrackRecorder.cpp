#include <tanh/audio-io/MultitrackRecorder.h>
#include <tanh/audio-io/Take.h>
#include <tanh/audio-io/WavStreamWriter.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/core/BufferView.h>
#include <tanh/core/threading/LockFreeQueue.h>
#include <tanh/core/threading/SpscRingBuffer.h>
#include <tanh/core/threading/Thread.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE::audio_io {

namespace {

constexpr size_t k_chunk_frames = 4096;
constexpr size_t k_segment_capacity = 4096;
constexpr auto k_idle_sleep = std::chrono::milliseconds(5);
constexpr auto k_data_sleep = std::chrono::milliseconds(1);
// How long the writer thread waits for the samples of a block the audio thread announced
// but has not finished copying, before it writes silence for them.
constexpr auto k_data_timeout = std::chrono::milliseconds(200);

// What the audio thread tells the writer thread, in the order it happened.
struct Segment {
    enum class Kind : uint8_t {
        Start,  // a take begins; where it starts in each buffer is in m_start_positions
        Data,   // m_frames frames follow in every track's buffer
        Gap,    // m_frames frames had no room: silence in every file
        End     // the take is over; m_timing is its timing
    };
    Kind m_kind = Kind::Data;
    uint32_t m_generation = 0;
    uint64_t m_frames = 0;
    TakeTiming m_timing{};
};

enum class Command : uint8_t { None, Finish, Cancel };

bool is_open_state(RecorderState state) {
    return state == RecorderState::Ready || state == RecorderState::Recording ||
           state == RecorderState::Finishing;
}

// A file name that is safe on every platform: letters, digits, space, '-', '_' and '.' stay.
std::string to_file_stem(const std::string& name) {
    std::string stem;
    stem.reserve(name.size());
    for (const char c : name) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '.';
        stem.push_back(safe ? c : '_');
    }
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) { stem.pop_back(); }
    while (!stem.empty() && (stem.front() == ' ' || stem.front() == '.')) { stem.erase(0, 1); }
    return stem.empty() ? std::string("track") : stem;
}

// to_file_stem() leaves ASCII only, so this is all the case folding a file system does to it.
std::string to_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return text;
}

// Distinct also where the file system ignores case (macOS, Windows): "Bass" and "bass" must
// not end up in one file.
std::vector<std::string> make_file_names(const std::vector<RecorderTrack>& tracks) {
    std::vector<std::string> names;
    std::vector<std::string> taken;  // lower case
    names.reserve(tracks.size());
    taken.reserve(tracks.size());
    for (const RecorderTrack& track : tracks) {
        const std::string stem = to_file_stem(track.m_name);
        std::string candidate = stem + ".wav";
        for (int suffix = 2; std::ranges::find(taken, to_lower(candidate)) != taken.end();
             ++suffix) {
            candidate = stem + "-" + std::to_string(suffix) + ".wav";
        }
        taken.push_back(to_lower(candidate));
        names.push_back(std::move(candidate));
    }
    return names;
}

}  // namespace

struct MultitrackRecorder::Impl {
    RecorderConfig m_config;
    bool m_prepared = false;
    size_t m_max_channels = 0;

    std::vector<std::unique_ptr<core::SpscRingBuffer<float>>> m_rings;
    std::unique_ptr<core::DynamicLockFreeQueue<Segment>> m_segments;

    // Shared.
    std::atomic<uint32_t> m_generation{0};         // the take the audio thread records
    std::atomic<uint32_t> m_closed_generation{0};  // the take it must stop feeding
    std::atomic<Command> m_command{Command::None};
    std::atomic<RecorderState> m_state{RecorderState::Idle};
    std::atomic<uint64_t> m_frames_recorded{0};
    std::atomic<uint64_t> m_dropped_frames{0};
    // Per prepared track. Which ones the take records: written by begin_take() before the
    // take's generation, read by the audio thread when it takes the generation up.
    std::vector<std::atomic<uint8_t>> m_selected;
    // Frames each buffer had received when the take began: written by the audio thread
    // before its Start message, read by the writer thread on that message.
    std::vector<std::atomic<uint64_t>> m_start_positions;

    // Control thread.
    uint32_t m_next_generation = 0;
    TakeTiming m_finish_timing;  // published by the store of Command::Finish
    TakeCallback m_callback;
    mutable std::mutex m_take_mutex;
    std::optional<Take> m_last_take;
    core::Thread m_thread;

    // Writer thread (set up by begin_take() before the thread starts).
    Take m_take;
    std::vector<WavStreamWriter> m_writers;  // one per track of the take
    std::vector<size_t> m_take_rings;        // the prepared track behind each of them
    std::vector<uint8_t> m_published;        // the file is at its path: this take put it there
    bool m_manifest_written = false;
    bool m_directory_created = false;  // by begin_take(): only then is it this take's to remove
    std::vector<uint64_t> m_ring_read_total;  // frames taken out of each buffer since prepare()
    std::vector<float> m_planar;              // m_max_channels x k_chunk_frames
    std::vector<float*> m_planar_pointers;
    std::vector<float> m_interleaved;  // k_chunk_frames x m_max_channels
    bool m_stalled = false;            // the audio thread stopped in the middle of a block

    // Audio thread.
    uint32_t m_rt_generation = 0;
    bool m_rt_open = false;
    bool m_rt_end_pending = false;
    bool m_rt_accepted = false;
    bool m_rt_final = false;
    uint32_t m_rt_begin = 0;
    uint32_t m_rt_length = 0;
    uint64_t m_rt_pending_gap = 0;
    std::vector<uint64_t> m_rt_ring_totals;  // frames each buffer has received since prepare()
    std::vector<uint8_t> m_rt_selected;      // the tracks of the take being recorded
    uint64_t m_rt_frames = 0;
    uint64_t m_rt_dropped = 0;
    TakeTiming m_rt_timing;
    std::vector<uint8_t> m_rt_written;
    std::vector<const float*> m_rt_pointers;

    // ---- audio thread ----------------------------------------------------------------------

    void rt_flush_gap() noexcept TANH_NONBLOCKING_FUNCTION {
        if (m_rt_pending_gap == 0) { return; }
        const Segment gap{.m_kind = Segment::Kind::Gap,
                          .m_generation = m_rt_generation,
                          .m_frames = m_rt_pending_gap};
        if (m_segments->try_push(gap)) { m_rt_pending_gap = 0; }
    }

    void rt_try_end() noexcept TANH_NONBLOCKING_FUNCTION {
        rt_flush_gap();
        const Segment end{.m_kind = Segment::Kind::End,
                          .m_generation = m_rt_generation,
                          .m_frames = 0,
                          .m_timing = m_rt_timing};
        if (m_rt_pending_gap == 0 && m_segments->try_push(end)) {
            m_rt_open = false;
            m_rt_end_pending = false;
        } else {
            m_rt_end_pending = true;
        }
    }

    // ---- writer thread ---------------------------------------------------------------------

    [[nodiscard]] uint64_t frames_left() const {
        uint64_t left = UINT64_MAX;
        for (const WavStreamWriter& writer : m_writers) {
            left = std::min(left, writer.frames_left());
        }
        return left;
    }

    // Moves `frames` frames of every track from its buffer to its file; at most `keep` of
    // them are written (the rest no longer fit the files). False when a write failed or the
    // take was cancelled.
    bool write_data(const core::Thread& self, uint64_t frames, uint64_t keep) {
        for (size_t track = 0; track < m_writers.size(); ++track) {
            const size_t ring = m_take_rings[track];
            const size_t channels = m_config.m_tracks[ring].m_channels;
            const std::span<float* const> pointers(m_planar_pointers.data(), channels);
            uint64_t done = 0;
            auto waiting_since = std::chrono::steady_clock::now();
            while (done < frames) {
                const auto wanted =
                    static_cast<size_t>(std::min<uint64_t>(k_chunk_frames, frames - done));
                const size_t got = m_rings[ring]->read(pointers, wanted);
                if (got == 0) {
                    // The block was announced and its samples are on their way: the audio
                    // thread may take as long as it likes over a block (an offline render, a
                    // debugger). Only a request to finish stops the waiting.
                    if (cancelled(self)) { return false; }
                    const bool finishing = m_command.load() == Command::Finish;
                    if (m_stalled ||
                        (finishing &&
                         std::chrono::steady_clock::now() - waiting_since > k_data_timeout)) {
                        // They never came (the audio stopped inside a block): silence keeps
                        // the tracks the same length, and the take ends here.
                        m_stalled = true;
                        const uint64_t missing = frames - done;
                        const uint64_t silent = done < keep ? std::min(missing, keep - done) : 0;
                        if (!m_writers[track].write_silence(silent)) { return false; }
                        break;
                    }
                    std::this_thread::sleep_for(k_data_sleep);
                    continue;
                }
                waiting_since = std::chrono::steady_clock::now();
                m_ring_read_total[ring] += got;

                const uint64_t writable = done < keep ? std::min<uint64_t>(got, keep - done) : 0;
                if (writable > 0) {
                    for (size_t channel = 0; channel < channels; ++channel) {
                        const float* samples = m_planar_pointers[channel];
                        for (size_t frame = 0; frame < writable; ++frame) {
                            m_interleaved[(frame * channels) + channel] = samples[frame];
                        }
                    }
                    if (!m_writers[track].write(m_interleaved.data(), writable)) { return false; }
                }
                done += got;
            }
        }
        return true;
    }

    bool write_gap(uint64_t frames) {
        for (WavStreamWriter& writer : m_writers) {
            if (!writer.write_silence(frames)) { return false; }
        }
        return true;
    }

    // The take's Start message arrived: each of its buffers is read from where the audio
    // thread says the take begins, whatever an earlier take left before that.
    void align_buffers() {
        for (const size_t ring : m_take_rings) {
            const uint64_t start = m_start_positions[ring].load(std::memory_order_relaxed);
            if (start > m_ring_read_total[ring]) {
                m_ring_read_total[ring] +=
                    m_rings[ring]->skip(static_cast<size_t>(start - m_ring_read_total[ring]));
            }
        }
    }

    // Control thread, in begin_take() while no writer thread runs and the audio thread does
    // not know the new take yet: make room. What is in the buffers now is an earlier take's;
    // align_buffers() skips to the exact start later anyway.
    void discard_leftovers() {
        for (size_t ring = 0; ring < m_rings.size(); ++ring) {
            m_ring_read_total[ring] += m_rings[ring]->skip(m_rings[ring]->available());
        }
    }

    [[nodiscard]] bool cancelled(const core::Thread& self) const {
        return self.should_stop() || m_command.load() == Command::Cancel;
    }

    // Removes what this take put on disk, and nothing else: a file that was at one of its
    // paths before (an earlier take in the same directory) is only replaced when this take
    // publishes its own, and stays when the take is dropped before that.
    void remove_files() {
        std::error_code error;
        for (WavStreamWriter& writer : m_writers) { writer.abort(); }
        for (size_t track = 0; track < m_published.size(); ++track) {
            if (m_published[track] != 0) {
                std::filesystem::remove(m_take.track_path(track), error);
                m_published[track] = 0;
            }
        }
        const std::filesystem::path directory(m_take.m_directory);
        if (m_manifest_written) {
            std::filesystem::remove(directory / Take::k_manifest_name, error);
            m_manifest_written = false;
        }
        if (m_directory_created) {
            std::filesystem::remove(directory, error);  // only when nothing else is in it
        }
    }

    void drop(RecorderState state) {
        remove_files();
        m_state.store(state);
    }

    void fail(uint32_t generation) {
        m_closed_generation.store(generation);
        drop(RecorderState::Failed);
        if (m_callback) { m_callback(m_take, false); }
    }

    void finish(const core::Thread& self,
                uint32_t generation,
                const TakeTiming& timing,
                bool truncated) {
        m_closed_generation.store(generation);
        m_state.store(RecorderState::Finishing);

        m_take.m_num_frames = m_writers.empty() ? 0 : m_writers.front().frames_written();
        m_take.m_dropped_frames = m_dropped_frames.load();
        m_take.m_truncated = truncated;
        m_take.m_timing = timing;
        m_take.m_timing.m_length_frames =
            std::min(m_take.m_timing.m_length_frames, m_take.m_num_frames);

        if (cancelled(self)) {
            drop(RecorderState::Idle);
            return;
        }
        bool closed = true;
        for (size_t track = 0; track < m_writers.size(); ++track) {
            if (m_writers[track].close()) {
                m_published[track] = 1;
            } else {
                closed = false;
            }
        }
        if (!closed) {
            fail(generation);
            return;
        }
        if (!save_take(m_take)) {
            // The audio is complete and on disk: it stays, only the manifest is missing.
            m_state.store(RecorderState::Failed);
            if (m_callback) { m_callback(m_take, false); }
            return;
        }
        m_manifest_written = true;
        {
            // With cancel_take(): either it finds the take done, or the take sees the cancel.
            const std::scoped_lock lock(m_take_mutex);
            if (cancelled(self)) {
                drop(RecorderState::Idle);
                return;
            }
            m_last_take = m_take;
            m_state.store(RecorderState::Done);
        }
        if (m_callback) { m_callback(m_take, true); }
    }

    void run(const core::Thread& self, uint32_t generation) {
        bool ended = false;
        bool truncated = false;
        TakeTiming timing;

        while (!ended) {
            if (cancelled(self)) {
                drop(RecorderState::Idle);
                return;
            }
            // Read before draining: what was queued before the request still belongs.
            const bool finish_requested = m_command.load() == Command::Finish;

            Segment segment;
            while (!ended && !m_stalled && !cancelled(self) && m_segments->try_pop(segment)) {
                if (segment.m_generation != generation) { continue; }  // an earlier take's
                bool written = true;
                switch (segment.m_kind) {
                    case Segment::Kind::Start: align_buffers(); break;
                    case Segment::Kind::Data:
                    case Segment::Kind::Gap: {
                        m_state.store(RecorderState::Recording);
                        const uint64_t keep = std::min(segment.m_frames, frames_left());
                        written = segment.m_kind == Segment::Kind::Data
                                      ? write_data(self, segment.m_frames, keep)
                                      : write_gap(keep);
                        if (written && keep < segment.m_frames) {
                            truncated = true;  // a file is full
                            ended = true;
                        }
                        break;
                    }
                    case Segment::Kind::End:
                        timing = segment.m_timing;
                        ended = true;
                        break;
                }
                if (!written) {
                    if (cancelled(self)) {
                        drop(RecorderState::Idle);
                    } else {
                        fail(generation);
                    }
                    return;
                }
            }

            if (!ended && (finish_requested || m_stalled)) {
                timing = m_finish_timing;
                ended = true;
            }
            if (!ended) { std::this_thread::sleep_for(k_idle_sleep); }
        }
        finish(self, generation, timing, truncated);
    }
};

MultitrackRecorder::MultitrackRecorder() : m_impl(std::make_unique<Impl>()) {}

MultitrackRecorder::~MultitrackRecorder() {
    cancel_take();
    m_impl->m_thread.join();
}

bool MultitrackRecorder::prepare(const RecorderConfig& config) {
    if (config.m_tracks.empty() || !std::isfinite(config.m_sample_rate) ||
        config.m_sample_rate < 1.0 || config.m_max_block_size == 0) {
        return false;
    }
    size_t max_channels = 0;
    for (const RecorderTrack& track : config.m_tracks) {
        if (track.m_channels == 0) { return false; }
        max_channels = std::max<size_t>(max_channels, track.m_channels);
    }

    cancel_take();
    Impl& impl = *m_impl;
    impl.m_thread.join();
    impl.m_generation.store(0);
    impl.m_state.store(RecorderState::Idle);
    impl.m_prepared = false;

    const double seconds = std::isfinite(config.m_buffer_seconds) ? config.m_buffer_seconds : 0.0;
    const auto wanted = static_cast<size_t>(std::max(0.0, seconds) * config.m_sample_rate);
    const size_t ring_frames = std::max<size_t>(wanted, size_t{2} * config.m_max_block_size);

    impl.m_config = config;
    impl.m_max_channels = max_channels;
    impl.m_rings.clear();
    for (const RecorderTrack& track : config.m_tracks) {
        auto ring = std::make_unique<core::SpscRingBuffer<float>>();
        ring->prepare(track.m_channels, ring_frames);
        impl.m_rings.push_back(std::move(ring));
    }
    impl.m_segments = std::make_unique<core::DynamicLockFreeQueue<Segment>>(k_segment_capacity);

    impl.m_selected = std::vector<std::atomic<uint8_t>>(config.m_tracks.size());
    impl.m_start_positions = std::vector<std::atomic<uint64_t>>(config.m_tracks.size());
    impl.m_take_rings.clear();
    impl.m_ring_read_total.assign(config.m_tracks.size(), 0);
    impl.m_planar.assign(max_channels * k_chunk_frames, 0.0f);
    impl.m_planar_pointers.resize(max_channels);
    for (size_t channel = 0; channel < max_channels; ++channel) {
        impl.m_planar_pointers[channel] = impl.m_planar.data() + (channel * k_chunk_frames);
    }
    impl.m_interleaved.assign(k_chunk_frames * max_channels, 0.0f);

    impl.m_rt_generation = 0;
    impl.m_rt_open = false;
    impl.m_rt_end_pending = false;
    impl.m_rt_accepted = false;
    impl.m_rt_final = false;
    impl.m_rt_pending_gap = 0;
    impl.m_rt_ring_totals.assign(config.m_tracks.size(), 0);
    impl.m_rt_selected.assign(config.m_tracks.size(), 0);
    impl.m_rt_written.assign(config.m_tracks.size(), 0);
    impl.m_rt_pointers.assign(max_channels, nullptr);

    impl.m_prepared = true;
    return true;
}

const RecorderConfig& MultitrackRecorder::config() const noexcept {
    return m_impl->m_config;
}

void MultitrackRecorder::set_take_callback(TakeCallback callback) {
    if (is_open_state(m_impl->m_state.load())) { return; }
    m_impl->m_thread.join();
    m_impl->m_callback = std::move(callback);
}

bool MultitrackRecorder::begin_take(const std::string& directory,
                                    SampleFormat format,
                                    const std::vector<size_t>& tracks) {
    Impl& impl = *m_impl;
    if (!impl.m_prepared || is_open_state(impl.m_state.load())) { return false; }

    // The tracks of this take, in the order they were prepared in.
    const size_t num_tracks = impl.m_config.m_tracks.size();
    std::vector<size_t> rings;
    for (size_t track = 0; track < num_tracks; ++track) {
        if (tracks.empty() || std::ranges::find(tracks, track) != tracks.end()) {
            rings.push_back(track);
        }
    }
    if (std::ranges::any_of(tracks, [num_tracks](size_t track) { return track >= num_tracks; })) {
        return false;
    }

    impl.m_thread.join();  // the last take's thread has returned or is about to
    impl.discard_leftovers();

    std::error_code error;
    const bool created = std::filesystem::create_directories(directory, error);
    if (error) { return false; }

    std::vector<RecorderTrack> sources;
    sources.reserve(rings.size());
    for (const size_t ring : rings) { sources.push_back(impl.m_config.m_tracks[ring]); }
    const std::vector<std::string> file_names = make_file_names(sources);
    impl.m_take = Take{};
    impl.m_take.m_directory = directory;
    impl.m_take.m_sample_rate = impl.m_config.m_sample_rate;
    impl.m_take.m_format = format;
    impl.m_writers.clear();
    impl.m_writers.resize(sources.size());
    impl.m_take_rings = rings;
    impl.m_published.assign(sources.size(), 0);
    impl.m_manifest_written = false;
    impl.m_directory_created = created;

    bool opened = true;
    for (size_t track = 0; track < sources.size() && opened; ++track) {
        const RecorderTrack& source = sources[track];
        impl.m_take.m_tracks.push_back({.m_name = source.m_name,
                                        .m_file = file_names[track],
                                        .m_channels = source.m_channels,
                                        .m_role = source.m_role});
        opened = impl.m_writers[track].open(impl.m_take.track_path(track),
                                            {.m_channels = source.m_channels,
                                             .m_sample_rate = impl.m_config.m_sample_rate,
                                             .m_format = format});
    }
    if (!opened) {
        impl.remove_files();
        return false;
    }

    for (size_t track = 0; track < num_tracks; ++track) {
        const bool selected = std::ranges::find(rings, track) != rings.end();
        impl.m_selected[track].store(selected ? 1 : 0, std::memory_order_relaxed);
    }
    impl.m_command.store(Command::None);
    impl.m_stalled = false;
    impl.m_frames_recorded.store(0);
    impl.m_dropped_frames.store(0);
    do { ++impl.m_next_generation; } while (impl.m_next_generation == 0);
    const uint32_t generation = impl.m_next_generation;

    impl.m_state.store(RecorderState::Ready);
    const bool started = impl.m_thread.start(
        {.m_priority = core::ThreadPriority::Normal, .m_name = "thl-recorder"},
        [&impl, generation](const core::Thread& self) { impl.run(self, generation); });
    if (!started) {
        impl.remove_files();
        impl.m_state.store(RecorderState::Idle);
        return false;
    }
    // Last: from here on the audio thread feeds the take.
    impl.m_generation.store(generation, std::memory_order_release);
    return true;
}

void MultitrackRecorder::end_take(const TakeTiming& timing) {
    Impl& impl = *m_impl;
    const RecorderState state = impl.m_state.load();
    if (state != RecorderState::Ready && state != RecorderState::Recording) { return; }
    if (impl.m_command.load() != Command::None) { return; }
    impl.m_finish_timing = timing;
    impl.m_closed_generation.store(impl.m_next_generation);
    impl.m_command.store(Command::Finish);
}

void MultitrackRecorder::cancel_take() {
    Impl& impl = *m_impl;
    {
        // With the writer thread's last step: a take that is not done yet sees the cancel.
        const std::scoped_lock lock(impl.m_take_mutex);
        if (!is_open_state(impl.m_state.load())) { return; }
        impl.m_closed_generation.store(impl.m_next_generation);
        impl.m_command.store(Command::Cancel);
    }
    impl.m_thread.join();
}

bool MultitrackRecorder::wait_until_finished(double timeout_seconds) {
    Impl& impl = *m_impl;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_seconds);
    while (is_open_state(impl.m_state.load())) {
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    impl.m_thread.join();  // the callback has returned too
    return true;
}

RecorderState MultitrackRecorder::state() const noexcept {
    return m_impl->m_state.load();
}

uint64_t MultitrackRecorder::frames_recorded() const noexcept {
    return m_impl->m_frames_recorded.load(std::memory_order_relaxed);
}

uint64_t MultitrackRecorder::dropped_frames() const noexcept {
    return m_impl->m_dropped_frames.load(std::memory_order_relaxed);
}

std::optional<Take> MultitrackRecorder::last_take() const {
    const std::scoped_lock lock(m_impl->m_take_mutex);
    return m_impl->m_last_take;
}

void MultitrackRecorder::begin_block(uint32_t num_samples,
                                     RecordSpan span) noexcept TANH_NONBLOCKING_FUNCTION {
    Impl& impl = *m_impl;
    impl.m_rt_accepted = false;
    impl.m_rt_final = false;
    impl.m_rt_length = 0;

    const uint32_t generation = impl.m_generation.load(std::memory_order_acquire);
    if (generation == 0) { return; }
    const bool closed = impl.m_closed_generation.load(std::memory_order_relaxed) == generation;

    if (generation != impl.m_rt_generation) {
        // A new take. Its first message says where in the buffers it starts.
        if (closed) { return; }
        for (size_t track = 0; track < impl.m_rings.size(); ++track) {
            impl.m_start_positions[track].store(impl.m_rt_ring_totals[track],
                                                std::memory_order_relaxed);
        }
        const Segment start{.m_kind = Segment::Kind::Start, .m_generation = generation};
        if (!impl.m_segments->try_push(start)) { return; }
        for (size_t track = 0; track < impl.m_rings.size(); ++track) {
            impl.m_rt_selected[track] = impl.m_selected[track].load(std::memory_order_relaxed);
        }
        impl.m_rt_generation = generation;
        impl.m_rt_open = true;
        impl.m_rt_end_pending = false;
        impl.m_rt_pending_gap = 0;
        impl.m_rt_frames = 0;
        impl.m_rt_dropped = 0;
        impl.m_rt_timing = {};
    }
    if (!impl.m_rt_open) { return; }
    if (closed) {
        impl.m_rt_open = false;
        return;
    }
    if (impl.m_rt_end_pending) {
        impl.rt_try_end();
        return;
    }

    const uint32_t begin = std::min(span.m_begin, num_samples);
    const uint32_t end = std::clamp(span.m_end, begin, num_samples);
    const uint32_t length = end - begin;
    if (length > 0) {
        bool room = true;
        for (size_t track = 0; room && track < impl.m_rings.size(); ++track) {
            room = impl.m_rt_selected[track] == 0 || impl.m_rings[track]->free_space() >= length;
        }
        // Lost blocks in a row stay one message: it is sent only when samples follow it.
        if (room) {
            impl.rt_flush_gap();
            room = impl.m_rt_pending_gap == 0;
        }
        // The announcement goes first: once it is queued, the samples must follow, and the
        // room for them was just checked.
        room = room && impl.m_segments->try_push(Segment{.m_kind = Segment::Kind::Data,
                                                         .m_generation = generation,
                                                         .m_frames = length});
        if (room) {
            impl.m_rt_accepted = true;
            impl.m_rt_begin = begin;
            impl.m_rt_length = length;
            std::ranges::fill(impl.m_rt_written, uint8_t{0});
        } else {
            impl.m_rt_pending_gap += length;
            impl.m_rt_dropped += length;
        }
        impl.m_rt_frames += length;
    }
    impl.m_rt_final = span.m_final;
}

void MultitrackRecorder::write(size_t track, const core::ConstBufferView& block) noexcept
    TANH_NONBLOCKING_FUNCTION {
    Impl& impl = *m_impl;
    if (!impl.m_rt_accepted || track >= impl.m_rings.size() || impl.m_rt_selected[track] == 0 ||
        impl.m_rt_written[track] != 0) {
        return;
    }
    if (block.get_num_samples() < size_t{impl.m_rt_begin} + impl.m_rt_length) { return; }

    const size_t channels = std::min(block.get_num_channels(), impl.m_rings[track]->num_channels());
    for (size_t channel = 0; channel < channels; ++channel) {
        impl.m_rt_pointers[channel] = block.get_read_pointer(channel) + impl.m_rt_begin;
    }
    impl.m_rings[track]->write(std::span<const float* const>(impl.m_rt_pointers.data(), channels),
                               impl.m_rt_length);
    impl.m_rt_written[track] = 1;
}

void MultitrackRecorder::set_timing(const TakeTiming& timing) noexcept TANH_NONBLOCKING_FUNCTION {
    m_impl->m_rt_timing = timing;
}

void MultitrackRecorder::end_block() noexcept TANH_NONBLOCKING_FUNCTION {
    Impl& impl = *m_impl;
    if (!impl.m_rt_open) { return; }
    if (impl.m_rt_accepted) {
        // Every buffer of the take gets the block's frames, written or not: that is what
        // keeps the tracks aligned.
        for (size_t track = 0; track < impl.m_rings.size(); ++track) {
            if (impl.m_rt_selected[track] == 0) { continue; }
            if (impl.m_rt_written[track] == 0) {
                impl.m_rings[track]->write({}, impl.m_rt_length);  // silence
            }
            impl.m_rt_ring_totals[track] += impl.m_rt_length;
        }
        impl.m_rt_accepted = false;
    }
    impl.m_frames_recorded.store(impl.m_rt_frames, std::memory_order_relaxed);
    impl.m_dropped_frames.store(impl.m_rt_dropped, std::memory_order_relaxed);
    if (impl.m_rt_final && !impl.m_rt_end_pending) { impl.rt_try_end(); }
}

}  // namespace thl::inline THL_ABI_NAMESPACE::audio_io
