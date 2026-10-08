#pragma once

#include <tanh/core/Exports.h>
#include <tanh/parameters/ParameterDefinitions.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace thl {

/**
 * @class ParameterNotFoundException
 * @brief Exception thrown when a parameter key is unknown to its store.
 *
 * Every parameter store reports a missing key with this type or a subclass
 * (thl::State throws StateKeyNotFoundException and StateGroupNotFoundException),
 * so code that works with any store catches this one.
 */
class TANH_API ParameterNotFoundException : public std::runtime_error {
public:
    explicit ParameterNotFoundException(std::string_view key)
        : ParameterNotFoundException(key, "Parameter not found: " + std::string(key)) {}
    ParameterNotFoundException(const ParameterNotFoundException&) = default;
    ParameterNotFoundException& operator=(const ParameterNotFoundException&) = default;
    ~ParameterNotFoundException() override;  // out of line: one typeinfo, in tanh::Parameters

    /// The key that was looked up.
    const std::string& key() const { return m_key; }

protected:
    ParameterNotFoundException(std::string_view key, const std::string& message)
        : std::runtime_error(message), m_key(key) {}

private:
    std::string m_key;
};

/**
 * @class ParameterTypeMismatchException
 * @brief Exception thrown when a requested type does not match the parameter's native type.
 */
class TANH_API ParameterTypeMismatchException : public std::runtime_error {
public:
    ParameterTypeMismatchException(ParameterType requested, ParameterType actual)
        : std::runtime_error(
              "ParameterHandle type mismatch: requested handle type does not match parameter type")
        , m_requested(requested)
        , m_actual(actual) {}
    ParameterTypeMismatchException(const ParameterTypeMismatchException&) = default;
    ParameterTypeMismatchException& operator=(const ParameterTypeMismatchException&) = default;
    ~ParameterTypeMismatchException() override;  // out of line: one typeinfo, in tanh::Parameters

    ParameterType requested() const { return m_requested; }
    ParameterType actual() const { return m_actual; }

private:
    ParameterType m_requested;
    ParameterType m_actual;
};

}  // namespace thl
