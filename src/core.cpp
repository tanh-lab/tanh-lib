// SPDX-License-Identifier: Apache-2.0
#include "tanh/core.h"

#include <tanh/core/AbiNamespace.h>

#include <string>

namespace thl::inline THL_ABI_NAMESPACE::core {

std::string get_version() {
#ifdef TANH_VERSION
    return TANH_VERSION;
#else
    return "unknown";
#endif
}

}  // namespace thl::inline THL_ABI_NAMESPACE::core
