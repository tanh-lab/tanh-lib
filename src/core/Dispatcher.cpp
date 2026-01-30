#include <tanh/core/Dispatcher.h>
#include <algorithm>

namespace thl {

void Dispatcher::add_listener(const std::string& event, DispatcherListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_listeners[event].push_back(listener);
}

void Dispatcher::remove_listener(DispatcherListener* listener) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& [event, listeners] : m_listeners) {
        listeners.erase(
            std::remove(listeners.begin(), listeners.end(), listener),
            listeners.end());
    }
}

void Dispatcher::dispatch(const std::string& event, const std::string& data) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_listeners.find(event);
    if (it != m_listeners.end()) {
        for (auto* listener : it->second) {
            listener->on_dispatch(event, data);
        }
    }
}

} // namespace thl
