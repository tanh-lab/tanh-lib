#include "HttpBackend.h"

namespace thl::net::detail {

// No native backend for this platform yet (Linux, Android). The component still
// links so callers compile everywhere; every request reports Unsupported, which
// HttpClient::supported() lets them check up front and degrade gracefully
// instead of discovering it per transfer.
HttpResult perform(const Request& /*request*/) {
    HttpResult result;
    result.m_status = HttpStatus::Unsupported;
    result.m_message = "No HTTP backend is compiled in for this platform.";
    return result;
}

bool backend_supported() {
    return false;
}

}  // namespace thl::net::detail
