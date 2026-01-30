#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace thl {

class DispatcherListener {
public:
    virtual void on_dispatch(const std::string& event, const std::string& data) = 0;
    virtual ~DispatcherListener() = default;
};

class Dispatcher {
public:
    /// Register a listener for a specific event.
    void add_listener(const std::string& event, DispatcherListener* listener);

    /// Remove a listener from all events it is registered on.
    void remove_listener(DispatcherListener* listener);

    /// Dispatch an event with data. Notifies all registered listeners.
    void dispatch(const std::string& event, const std::string& data);

private:
    std::mutex m_mutex;
    std::unordered_map<std::string, std::vector<DispatcherListener*>> m_listeners;
};

} // namespace thl
