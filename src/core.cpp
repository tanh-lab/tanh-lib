#include "tanh/core.h"
#include <string>

namespace thl::core {

std::string get_version() {
#ifdef TANH_VERSION
    return TANH_VERSION;
#else
    return "unknown";
#endif
}

}  // namespace thl::core
