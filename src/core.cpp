#include "tanh/core.h"
#include <stdexcept>
#include <sstream>

namespace thl {

namespace core {

std::string get_version() {
#ifdef TANH_VERSION
    return TANH_VERSION;
#else
    return "unknown";
#endif
}

}  // namespace core

}  // namespace thl
