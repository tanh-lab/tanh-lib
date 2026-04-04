#pragma once

#include <tanh/audio-io/DataSource.h>
#include "miniaudio.h"

namespace thl::audio_io {

struct DataSource::Impl {
    ma_resource_manager m_resource_manager{};
    ma_resource_manager_data_source m_data_source{};
    ma_decoder m_decoder{};
    bool m_resource_manager_initialised = false;
    bool m_data_source_initialised = false;
    bool m_decoder_initialised = false;
    uint32_t m_channels = 0;
    uint32_t m_sample_rate = 0;

    ma_data_source* get_data_source() {
        if (m_decoder_initialised) { return &m_decoder; }
        if (m_data_source_initialised) { return &m_data_source; }
        return nullptr;
    }

    bool is_valid() const { return m_data_source_initialised || m_decoder_initialised; }

    ~Impl() {
        if (m_decoder_initialised) { ma_decoder_uninit(&m_decoder); }
        if (m_data_source_initialised) { ma_resource_manager_data_source_uninit(&m_data_source); }
        if (m_resource_manager_initialised) { ma_resource_manager_uninit(&m_resource_manager); }
    }

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

}  // namespace thl::audio_io
