#include <tanh/core/Dispatcher.h>
#include <string>
#include <mutex>

namespace thl {

void Dispatcher::add_listener(const std::string& event, DispatcherListener* listener) {
    std::scoped_lock const lock(m_mutex);
    m_listeners[event].push_back(listener);
}

void Dispatcher::remove_listener(DispatcherListener* listener) {
    std::scoped_lock const lock(m_mutex);

    for (auto& [event, listeners] : m_listeners) { std::erase(listeners, listener); }
}

void Dispatcher::dispatch(const std::string& event, const std::string& data) {
    std::scoped_lock const lock(m_mutex);

    auto it = m_listeners.find(event);
    if (it != m_listeners.end()) {
        for (auto* listener : it->second) { listener->on_dispatch(event, data); }
    }
}

}  // namespace thl
