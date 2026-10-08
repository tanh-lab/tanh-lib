#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "tanh/core/Exports.h"
#include "tanh/core/parameters/ParameterExceptions.h"

namespace thl {

/**
 * @class StateKeyNotFoundException
 * @brief Exception thrown when a parameter key is not found in the state.
 */
class TANH_API StateKeyNotFoundException : public ParameterNotFoundException {
public:
    explicit StateKeyNotFoundException(std::string_view key)
        : ParameterNotFoundException(key, "Key not found in state: " + std::string(key)) {}
};

/**
 * @class StateGroupNotFoundException
 * @brief Exception thrown when a state group is not found.
 *
 * A ParameterNotFoundException whose key() is the missing group's name.
 */
class TANH_API StateGroupNotFoundException : public ParameterNotFoundException {
public:
    explicit StateGroupNotFoundException(std::string_view group_name)
        : ParameterNotFoundException(group_name,
                                     "State group not found: " + std::string(group_name)) {}
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

}  // namespace thl
