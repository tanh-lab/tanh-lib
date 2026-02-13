#include <tanh/audio_io/DataSource.h>
#include "DataSourceImpl.h"

namespace thl::audio_io {

DataSource::DataSource(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

DataSource::~DataSource() = default;
DataSource::DataSource(DataSource&&) noexcept = default;
DataSource& DataSource::operator=(DataSource&&) noexcept = default;

uint64_t DataSource::read_pcm_frames(float* output, uint64_t frame_count) {
    if (!m_impl || !m_impl->is_valid()) return 0;

    ma_uint64 frames_read = 0;
    ma_data_source_read_pcm_frames(m_impl->get_data_source(),
                                   output,
                                   frame_count,
                                   &frames_read);
    return frames_read;
}

bool DataSource::seek(uint64_t frame_index) {
    if (!m_impl || !m_impl->is_valid()) return false;

    return ma_data_source_seek_to_pcm_frame(m_impl->get_data_source(),
                                            frame_index) == MA_SUCCESS;
}

uint32_t DataSource::get_channel_count() const {
    if (!m_impl || !m_impl->is_valid()) return 0;
    return m_impl->channels;
}

uint32_t DataSource::get_sample_rate() const {
    if (!m_impl || !m_impl->is_valid()) return 0;
    return m_impl->sampleRate;
}

uint64_t DataSource::get_total_frames() const {
    if (!m_impl || !m_impl->is_valid()) return 0;

    ma_uint64 length = 0;
    ma_data_source_get_length_in_pcm_frames(m_impl->get_data_source(), &length);
    return length;
}

uint64_t DataSource::get_cursor() const {
    if (!m_impl || !m_impl->is_valid()) return 0;

    ma_uint64 cursor = 0;
    ma_data_source_get_cursor_in_pcm_frames(m_impl->get_data_source(), &cursor);
    return cursor;
}

bool DataSource::is_valid() const {
    return m_impl && m_impl->is_valid();
}

}  // namespace thl::audio_io
