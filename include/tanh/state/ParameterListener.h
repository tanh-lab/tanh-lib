#pragma once

#include <functional>

#include "tanh/core/Exports.h"

namespace thl {

// Forward declarations
class Parameter;

// Notification strategy — determines which listeners receive callbacks.
// Applied based on the source listener's m_strategy field when set() is called.
enum class NotifyStrategies { All, None, Others, Self };

/**
 * @class ParameterListener
 * @brief Abstract interface for receiving parameter change notifications.
 *
 * Implement this interface to receive callbacks when parameters are modified.
 * Listeners can be registered with StateGroup::add_listener().
 *
 * The m_strategy field determines how notifications are filtered when this
 * listener is the source of a set() call:
 * - All: notify everyone including this listener
 * - Others: notify everyone except this listener
 * - Self: only notify this listener
 * - None: skip all notifications
 */
class TANH_API ParameterListener {
public:
    /**
     * @brief Constructs a ParameterListener.
     *
     * @param strategy Notification strategy applied when this listener is
     *        the source of a set() call. Default: All (notify everyone).
     * @param receives_during_gesture If false, this listener will be skipped
     *        while a parameter is in an active user gesture (e.g., drag).
     *        Defaults to true (always receive notifications).
     */
    explicit ParameterListener(NotifyStrategies strategy = NotifyStrategies::All,
                               bool receives_during_gesture = true)
        : m_strategy(strategy), m_receives_during_gesture(receives_during_gesture) {}

    virtual ~ParameterListener() = default;

    /**
     * @brief Called when a parameter value changes.
     *
     * @param param The Parameter object with the new value.
     *        Use param.key() to get the full path.
     */
    virtual void on_parameter_changed(const Parameter& param) = 0;

    /**
     * @brief Called when a gesture begins on a parameter.
     * @param param The Parameter being gestured
     */
    virtual void on_gesture_start(const Parameter& param) { (void)param; }

    /**
     * @brief Called when a gesture ends on a parameter.
     * @param param The Parameter being gestured
     */
    virtual void on_gesture_end(const Parameter& param) { (void)param; }

    /// Notification strategy for this listener (applied when it is the source).
    NotifyStrategies m_strategy;
    /// Whether this listener receives notifications during active gestures.
    bool m_receives_during_gesture;
};

/**
 * @class CallbackListener
 * @brief Utility class wrapping a std::function in a ParameterListener.
 *
 * Use this instead of the removed add_callback_listener() API:
 * @code
 * CallbackListener cb([](const Parameter& p) { ... });
 * state.add_listener(&cb);
 * @endcode
 */
class TANH_API CallbackListener : public ParameterListener {
public:
    explicit CallbackListener(std::function<void(const Parameter&)> callback,
                              NotifyStrategies strategy = NotifyStrategies::All,
                              bool receives_during_gesture = true)
        : ParameterListener(strategy, receives_during_gesture), m_callback(std::move(callback)) {}

    void on_parameter_changed(const Parameter& param) override { m_callback(param); }

private:
    std::function<void(const Parameter&)> m_callback;
};

}  // namespace thl
