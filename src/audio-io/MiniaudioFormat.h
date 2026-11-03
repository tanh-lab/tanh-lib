#pragma once

#include <tanh/audio-io/AudioEncodingFormat.h>
#include <tanh/core/AbiNamespace.h>

#include "miniaudio.h"

namespace thl::inline THL_ABI_NAMESPACE {

inline ma_encoding_format to_miniaudio_format(AudioEncodingFormat format) {
    switch (format) {
        case AudioEncodingFormat::WAV: return ma_encoding_format_wav;
        case AudioEncodingFormat::FLAC: return ma_encoding_format_flac;
    }
    return ma_encoding_format_wav;
}

}  // namespace thl::inline THL_ABI_NAMESPACE
