// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "core/Buffer.h"
#include "core/BufferView.h"
#include "core/Dispatcher.h"
#include "core/Logger.h"
#include "core/MemoryBlock.h"
#include "core/RingBuffer.h"
#include "core/RtFormat.h"
#include "core/threading/RCU.h"

// Core utility functions available to all components
namespace thl::core {
/**
 * @brief Get the library version
 */
std::string get_version();

}  // namespace thl::core
