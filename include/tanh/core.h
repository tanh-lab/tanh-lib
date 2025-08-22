#pragma once

#include "core/threading/RCU.h"

#include <string>

namespace thl {

// Core utility functions available to all components
namespace core {
    /**
     * @brief Get the library version
     */
    std::string getVersion();
}

} // namespace thl
