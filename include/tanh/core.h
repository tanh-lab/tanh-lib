// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/core/AbiNamespace.h>

#include <string>

#include "core/Buffer.h"
#include "core/BufferView.h"
#include "core/Dispatcher.h"
#include "core/Exports.h"
#include "core/Logger.h"
#include "core/MemoryBlock.h"
#include "core/RingBuffer.h"
#include "core/RtFormat.h"
#include "core/WavReader.h"
#include "core/threading/LockFreeQueue.h"
#include "core/threading/RCU.h"
#include "core/threading/Thread.h"

// Core utility functions available to all components
namespace thl::inline THL_ABI_NAMESPACE::core {
/**
 * @brief Get the library version
 */
TANH_API std::string get_version();

}  // namespace thl::inline THL_ABI_NAMESPACE::core
