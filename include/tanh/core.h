#pragma once

#include <string>

#include "core/Dispatcher.h"
#include "core/Logger.h"
#include "core/threading/RCU.h"

// Core utility functions available to all components
namespace thl::core {
/**
 * @brief Get the library version
 */
std::string get_version();

}  // namespace thl::core
