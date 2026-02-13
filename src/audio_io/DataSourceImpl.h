#pragma once

#include <tanh/audio_io/DataSource.h>
#include "miniaudio.h"

namespace thl::audio_io {

struct DataSource::Impl {
    ma_resource_manager resourceManager{};
    ma_resource_manager_data_source dataSource{};
    ma_decoder decoder{};
    bool resourceManagerInitialised = false;
    bool dataSourceInitialised = false;
    bool decoderInitialised = false;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;

    ma_data_source* get_data_source() {
        if (decoderInitialised) return &decoder;
        if (dataSourceInitialised) return &dataSource;
        return nullptr;
    }

    bool is_valid() const {
        return dataSourceInitialised || decoderInitialised;
    }

    ~Impl() {
        if (decoderInitialised) {
            ma_decoder_uninit(&decoder);
        }
        if (dataSourceInitialised) {
            ma_resource_manager_data_source_uninit(&dataSource);
        }
        if (resourceManagerInitialised) {
            ma_resource_manager_uninit(&resourceManager);
        }
    }

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

}  // namespace thl::audio_io
