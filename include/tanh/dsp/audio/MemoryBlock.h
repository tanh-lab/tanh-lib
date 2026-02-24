#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <tanh/core/Logger.h>
#include <type_traits>
#include <utility>

namespace thl::dsp::audio {

template <typename T>
class MemoryBlock {
public:
    MemoryBlock(std::size_t size = 0) : m_size(size) {
        if (m_size > 0) {
            void* data = std::malloc(sizeof(T) * m_size);
            if (data != nullptr) {
                m_data = static_cast<T*>(data);
            } else {
                thl::Logger::error("thl.dsp.audio.memory_block", "MemoryBlock: failed to allocate memory");
            }
        }
    }

    ~MemoryBlock() noexcept { std::free(m_data); }

    MemoryBlock(const MemoryBlock& other) : m_size(other.m_size) {
        if (m_size > 0) {
            void* data = std::malloc(sizeof(T) * m_size);
            if (data != nullptr) {
                m_data = static_cast<T*>(data);
                std::memcpy(m_data, other.m_data, sizeof(T) * m_size);
            } else {
                thl::Logger::error("thl.dsp.audio.memory_block", "MemoryBlock: failed to allocate memory");
            }
        }
    }

    MemoryBlock& operator=(const MemoryBlock& other) {
        if (this != &other) {
            std::free(m_data);
            m_data = nullptr;
            m_size = other.m_size;
            if (m_size > 0) {
                void* data = std::malloc(sizeof(T) * m_size);
                if (data != nullptr) {
                    m_data = static_cast<T*>(data);
                    std::memcpy(m_data, other.m_data, sizeof(T) * m_size);
                } else {
                    thl::Logger::error("thl.dsp.audio.memory_block", "MemoryBlock: failed to allocate memory");
                }
            }
        }
        return *this;
    }

    MemoryBlock(MemoryBlock&& other) noexcept
        : m_data(other.m_data), m_size(other.m_size) {
        other.m_size = 0;
        other.m_data = nullptr;
    }

    MemoryBlock& operator=(MemoryBlock&& other) noexcept {
        if (this != &other) {
            std::free(m_data);
            m_size = other.m_size;
            m_data = other.m_data;
            other.m_size = 0;
            other.m_data = nullptr;
        }
        return *this;
    }

    T& operator[](size_t index) { return m_data[index]; }
    const T& operator[](size_t index) const { return m_data[index]; }

    T* data() { return m_data; }
    const T* data() const { return m_data; }
    size_t size() const { return m_size; }

    void resize(size_t size) {
        m_size = size;
        void* data;
        if (m_data != nullptr) {
            data = std::realloc(m_data, sizeof(T) * size);
        } else {
            data = std::malloc(sizeof(T) * size);
        }

        if (data != nullptr) {
            m_data = static_cast<T*>(data);
        } else {
            thl::Logger::error("thl.dsp.audio.memory_block", "MemoryBlock: failed to reallocate memory");
        }
    }

    void clear() { std::memset(m_data, 0, sizeof(T) * m_size); }

    template <typename U = T,
              std::enable_if_t<std::is_trivially_copyable_v<U>, bool> = true>
    void swap_data(MemoryBlock& other) {
        if (this != &other) {
            if (m_size == other.m_size) {
                std::swap(m_data, other.m_data);
            } else {
                    thl::Logger::error(
                    "thl.dsp.audio.memory_block",
                    "MemoryBlock: cannot swap data with different sizes");
            }
        }
    }

    void swap_data(T*& data, size_t size) {
        if (m_size == size) {
            std::swap(m_data, data);
        } else {
            thl::Logger::error(
                "thl.dsp.audio.memory_block",
                "MemoryBlock: cannot swap data with different sizes");
        }
    }

private:
    T* m_data = nullptr;
    size_t m_size = 0;
};

}  // namespace thl::dsp::audio
