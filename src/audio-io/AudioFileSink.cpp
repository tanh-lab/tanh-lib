#include <tanh/audio-io/AudioEncodingFormat.h>
#include <tanh/audio-io/AudioFileSink.h>
#include <tanh/audio-io/WavStreamWriter.h>
#include <tanh/core/AbiNamespace.h>
#include <tanh/core/threading/SpscRingBuffer.h>
#include <tanh/core/threading/Thread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace thl::inline THL_ABI_NAMESPACE {

namespace {

// Audio the sink holds between the audio thread and the disk.
constexpr double k_buffer_seconds = 2.0;
constexpr size_t k_min_buffer_frames = 16384;
constexpr size_t k_write_frames = 4096;
constexpr auto k_idle_sleep = std::chrono::milliseconds(5);

}  // namespace

struct AudioFileSink::Impl {
    // One channel of interleaved samples: a block is published whole, so what the writer
    // thread finds is always a whole number of frames.
    core::SpscRingBuffer<float> m_ring;
    audio_io::WavStreamWriter m_writer;
    std::vector<float> m_scratch;
    core::Thread m_thread;  // after what it uses: joined before those go
    uint32_t m_channels = 0;

    // The file takes no more: a write failed, or it is full. The audio thread then counts
    // what it is handed as dropped instead of buffering it for nothing.
    std::atomic<bool> m_refusing{false};
    std::atomic<bool> m_write_failed{false};

    // Writer thread: moves what has arrived to the file. Returns the samples moved.
    size_t drain() {
        float* destination = m_scratch.data();
        const size_t count =
            m_ring.read(std::span<float* const>(&destination, 1), m_scratch.size());
        if (count == 0 || m_refusing.load(std::memory_order_relaxed)) { return count; }

        // A file holds 4 GiB: what fits is written, and the recording ends there.
        const uint64_t frames = count / m_channels;
        const uint64_t fitting = std::min(frames, m_writer.frames_left());
        if (!m_writer.write(m_scratch.data(), fitting)) {
            m_write_failed.store(true, std::memory_order_relaxed);
            m_refusing.store(true, std::memory_order_relaxed);
        } else if (fitting < frames) {
            m_refusing.store(true, std::memory_order_relaxed);
        }
        return count;
    }
};

AudioFileSink::AudioFileSink() : m_impl(std::make_unique<Impl>()) {}

AudioFileSink::~AudioFileSink() {
    close_file();
}

bool AudioFileSink::open_file(const std::string& file_path,
                              uint32_t channels,
                              uint32_t sample_rate,
                              AudioEncodingFormat format) {
    close_file();
    if (format != AudioEncodingFormat::WAV) { return false; }

    if (!m_impl->m_writer.open(file_path,
                               {.m_channels = channels,
                                .m_sample_rate = static_cast<double>(sample_rate),
                                .m_format = audio_io::SampleFormat::Float32})) {
        return false;
    }

    const auto buffer_frames =
        std::max(k_min_buffer_frames, static_cast<size_t>(k_buffer_seconds * sample_rate));
    m_impl->m_channels = channels;
    m_impl->m_ring.prepare(1, buffer_frames * channels);
    m_impl->m_scratch.assign(k_write_frames * channels, 0.0f);
    m_impl->m_refusing.store(false, std::memory_order_relaxed);
    m_impl->m_write_failed.store(false, std::memory_order_relaxed);
    m_frames_written.store(0, std::memory_order_release);
    m_frames_dropped.store(0, std::memory_order_release);

    Impl& impl = *m_impl;
    const bool started =
        impl.m_thread.start({.m_priority = core::ThreadPriority::Normal, .m_name = "thl-file-sink"},
                            [&impl](const core::Thread& self) {
                                // A stop request still writes everything that has arrived.
                                while (true) {
                                    if (impl.drain() > 0) { continue; }
                                    if (self.should_stop()) { break; }
                                    std::this_thread::sleep_for(k_idle_sleep);
                                }
                            });
    if (!started) {
        m_impl->m_writer.abort();
        m_impl->m_channels = 0;
        return false;
    }
    m_open = true;
    return true;
}

bool AudioFileSink::close_file() {
    if (!m_open) { return false; }
    m_recording.store(false, std::memory_order_release);
    m_open = false;
    m_impl->m_thread.request_stop();
    m_impl->m_thread.join();
    // A failed write makes the writer drop the file; so does a header it cannot finish.
    const bool closed = m_impl->m_writer.close();
    m_impl->m_channels = 0;
    if (!closed) { m_impl->m_write_failed.store(true, std::memory_order_relaxed); }
    return closed;
}

bool AudioFileSink::has_failed() const {
    return m_impl->m_write_failed.load(std::memory_order_relaxed);
}

void AudioFileSink::start_recording() {
    if (m_open) {
        m_peak_level.store(0.0f, std::memory_order_release);
        m_recording.store(true, std::memory_order_release);
    }
}

void AudioFileSink::stop_recording() {
    m_recording.store(false, std::memory_order_release);
}

void AudioFileSink::process(float* /*outputBuffer*/,
                            const float* input_buffer,
                            uint32_t frame_count,
                            uint32_t num_input_channels,
                            uint32_t /*numOutputChannels*/
) {
    if (!input_buffer || num_input_channels == 0) { return; }
    if (!m_open || !m_recording.load(std::memory_order_acquire)) { return; }
    if (num_input_channels != m_impl->m_channels) { return; }

    // A block goes in whole or not at all, so the file never holds half a frame.
    const size_t total_samples = static_cast<size_t>(frame_count) * num_input_channels;
    if (m_impl->m_refusing.load(std::memory_order_relaxed) ||
        m_impl->m_ring.free_space() < total_samples) {
        m_frames_dropped.fetch_add(frame_count, std::memory_order_relaxed);
        return;
    }
    m_impl->m_ring.write(std::span<const float* const>(&input_buffer, 1), total_samples);
    m_frames_written.fetch_add(frame_count, std::memory_order_relaxed);

    // Compute peak amplitude for this block
    float peak = 0.0f;
    for (size_t i = 0; i < total_samples; ++i) {
        float const abs_val = std::fabs(input_buffer[i]);
        if (abs_val > peak) { peak = abs_val; }
    }
    m_peak_level.store(peak, std::memory_order_release);
}

void AudioFileSink::release_resources() {
    close_file();
}

}  // namespace thl::inline THL_ABI_NAMESPACE
