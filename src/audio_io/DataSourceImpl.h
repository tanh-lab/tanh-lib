#pragma once

#include <tanh/audio_io/DataSource.h>
#include "miniaudio.h"

namespace thl::audio_io {

struct DataSource::Impl {
    ma_resource_manager resourceManager{};
    ma_resource_manager_data_source dataSource{};
    bool resourceManagerInitialised = false;
    bool dataSourceInitialised = false;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;

    ~Impl() {
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
