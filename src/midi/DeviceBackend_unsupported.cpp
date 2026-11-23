#include <tanh/core/AbiNamespace.h>
#include <tanh/midi/DeviceInfo.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "DeviceBackend.h"

namespace thl::inline THL_ABI_NAMESPACE::midi {

namespace {

// A platform without a device backend: no inputs, nothing opens. The queue still works
// for packets the application pushes itself.
class UnsupportedBackend final : public DeviceBackend {
public:
    [[nodiscard]] bool running() const override { return false; }
    [[nodiscard]] std::vector<DeviceInfo> input_devices() const override { return {}; }
    bool open_input(std::uint32_t /*id*/) override { return false; }
    bool close_input(std::uint32_t /*id*/) override { return false; }
    void set_auto_open(bool /*auto_open*/) override {}
    bool create_virtual_input(const std::string& /*name*/) override { return false; }
};

}  // namespace

bool device_backend_supported() {
    return false;
}

// The signature is the one every backend shares.
// NOLINTNEXTLINE(performance-unnecessary-value-param)
std::unique_ptr<DeviceBackend> make_device_backend(BackendContext /*context*/) {
    return std::make_unique<UnsupportedBackend>();
}

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
