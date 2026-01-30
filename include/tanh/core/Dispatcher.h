#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace thl {

class Dispatcher {
public:
    class Listener {
    public:
        virtual void on_dispatch(const std::string& event, const std::string& data) = 0;
        virtual ~Listener() = default;
    };

    /// Register a listener for a specific event.
    void add_listener(const std::string& event, Listener* listener);

    /// Remove a listener from all events it is registered on.
    void remove_listener(Listener* listener);

    /// Dispatch an event with data. Notifies all registered listeners.
    void dispatch(const std::string& event, const std::string& data);

private:
    std::mutex m_mutex;
    std::unordered_map<std::string, std::vector<Listener*>> m_listeners;
};

} // namespace thl
