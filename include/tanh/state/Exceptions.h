#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ParameterDefinitions.h"
#include "tanh/core/Exports.h"

namespace thl {

/**
 * @class StateKeyNotFoundException
 * @brief Exception thrown when a parameter key is not found in the state.
 */
class TANH_API StateKeyNotFoundException : public std::runtime_error {
public:
    explicit StateKeyNotFoundException(std::string_view key)
        : std::runtime_error("Key not found in state: " + std::string(key)), m_key(key) {}

    const std::string& key() const { return m_key; }

private:
    std::string m_key;
};

/**
 * @class StateGroupNotFoundException
 * @brief Exception thrown when a state group is not found.
 */
class TANH_API StateGroupNotFoundException : public std::runtime_error {
public:
    explicit StateGroupNotFoundException(std::string_view group_name)
        : std::runtime_error("State group not found: " + std::string(group_name))
        , m_group_name(group_name) {}

private:
    std::string m_group_name;
};

/**
 * @class BlockingException
 * @brief Exception thrown when attempting a blocking operation without
 * allow_blocking flag.
 */
class TANH_API BlockingException : public std::runtime_error {
public:
    explicit BlockingException(std::string_view path)
        : std::runtime_error("Blocking operation requires allow_blocking=true: " +
                             std::string(path))
        , m_path(path) {}

    const std::string& path() const { return m_path; }

private:
    std::string m_path;
};

/**
 * @class ParameterAlreadyExistsException
 * @brief Exception thrown when attempting to create a parameter that already exists.
 */
class TANH_API ParameterAlreadyExistsException : public std::runtime_error {
public:
    explicit ParameterAlreadyExistsException(std::string_view key)
        : std::runtime_error("Parameter already exists: " + std::string(key)), m_key(key) {}

    const std::string& key() const { return m_key; }

private:
    std::string m_key;
};

/**
 * @class DuplicateParameterIdException
 * @brief Exception thrown when a parameter ID is already in use by another parameter.
 */
class TANH_API DuplicateParameterIdException : public std::runtime_error {
public:
    DuplicateParameterIdException(uint32_t id, std::string_view key)
        : std::runtime_error("Duplicate parameter ID " + std::to_string(id) + " for key '" +
                             std::string(key) + "'")
        , m_id(id)
        , m_key(key) {}

    uint32_t id() const { return m_id; }
    const std::string& key() const { return m_key; }

private:
    uint32_t m_id;
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

    ParameterType requested() const { return m_requested; }
    ParameterType actual() const { return m_actual; }

private:
    ParameterType m_requested;
    ParameterType m_actual;
};

}  // namespace thl
