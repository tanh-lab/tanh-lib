#pragma once

#include <tanh/core/AbiNamespace.h>

namespace thl::inline THL_ABI_NAMESPACE {

/**
 * @enum AudioEncodingFormat
 * @brief Audio file encoding formats.
 *
 * The bundled miniaudio encoder only writes WAV; requesting FLAC fails.
 */
enum class AudioEncodingFormat { WAV, FLAC };

}  // namespace thl::inline THL_ABI_NAMESPACE
