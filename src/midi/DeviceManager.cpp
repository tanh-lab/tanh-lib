#include <tanh/core/AbiNamespace.h>
#include <tanh/core/AtomicSharedPtr.h>
#include <tanh/midi/AndroidMidi.h>
#include <tanh/midi/DeviceInfo.h>
#include <tanh/midi/DeviceManager.h>
#include <tanh/midi/InputQueue.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "DeviceBackend.h"

namespace thl::inline THL_ABI_NAMESPACE::midi {

struct DeviceManager::Impl {
    // The callback slot outlives the manager for as long as the backend's threads may
    // still fire: they hold it through the shared pointer, never through the manager.
    struct CallbackSlot {
        AtomicSharedPtr<DeviceChangeCallback> m_callback;
    };

    std::shared_ptr<InputQueue> m_queue;
    std::shared_ptr<CallbackSlot> m_callback_slot = std::make_shared<CallbackSlot>();
    std::unique_ptr<DeviceBackend> m_backend;
};

DeviceManager::DeviceManager(const DeviceManagerConfig& config) : m_impl(std::make_unique<Impl>()) {
    m_impl->m_queue = std::make_shared<InputQueue>(config.m_queue_capacity);

    BackendContext context;
    context.m_queue = m_impl->m_queue;
    context.m_protocol = config.m_protocol;
    context.m_auto_open = config.m_auto_open;
    context.m_receive_sysex = config.m_receive_sysex;
    context.m_client_name = config.m_client_name;
    context.m_on_change = [slot = m_impl->m_callback_slot] {
        if (const auto callback = atomic_load(slot->m_callback)) { (*callback)(); }
    };
    m_impl->m_backend = make_device_backend(std::move(context));
}

DeviceManager::~DeviceManager() {
    // Clear first: a change the backend reports while it shuts down reaches nobody.
    atomic_store(m_impl->m_callback_slot->m_callback, std::shared_ptr<DeviceChangeCallback>{});
    m_impl->m_backend.reset();
}

bool DeviceManager::supported() {
    return device_backend_supported();
}

bool DeviceManager::running() const {
    return m_impl->m_backend->running();
}

std::vector<DeviceInfo> DeviceManager::input_devices() const {
    return m_impl->m_backend->input_devices();
}

bool DeviceManager::open_input(std::uint32_t id) {
    return m_impl->m_backend->open_input(id);
}

bool DeviceManager::close_input(std::uint32_t id) {
    return m_impl->m_backend->close_input(id);
}

void DeviceManager::set_auto_open(bool auto_open) {
    m_impl->m_backend->set_auto_open(auto_open);
}

void DeviceManager::set_device_change_callback(DeviceChangeCallback callback) {
    auto shared = callback ? std::make_shared<DeviceChangeCallback>(std::move(callback)) : nullptr;
    atomic_store(m_impl->m_callback_slot->m_callback, std::move(shared));
}

bool DeviceManager::create_virtual_input(const std::string& name) {
    return m_impl->m_backend->create_virtual_input(name);
}

InputQueue& DeviceManager::input_queue() noexcept {
    return *m_impl->m_queue;
}

#if !defined(THL_PLATFORM_ANDROID)
// The Android backend defines the real one (DeviceBackend_android.cpp).
void set_android_java_vm(void* /*java_vm*/) {}
#endif

}  // namespace thl::inline THL_ABI_NAMESPACE::midi
