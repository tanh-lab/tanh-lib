// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace thl::core {

/**
 * Contiguous, resizable block of raw storage for `size()` elements of T.
 *
 * Elements are never constructed or destroyed by the block: it hands out
 * malloc'd memory, which makes it usable for non-copyable element types
 * (e.g. std::atomic<float>) as long as the caller treats the storage as raw.
 *
 * Failure semantics (no logging, no platform dependencies):
 *  - Allocation failure throws std::bad_alloc and leaves the block unchanged.
 *  - swap_data() with a size mismatch is a contract violation: it asserts in
 *    debug builds and is a no-op in release builds.
 */
template <typename T>
class MemoryBlock {
public:
    MemoryBlock(std::size_t size = 0) : m_size(size) {
        if (m_size > 0) { m_data = allocate(m_size); }
    }

    ~MemoryBlock() noexcept { std::free(m_data); }

    MemoryBlock(const MemoryBlock& other) : m_size(other.m_size) {
        if (m_size > 0 && other.m_data != nullptr) {
            m_data = allocate(m_size);
            std::memcpy(m_data, other.m_data, sizeof(T) * m_size);
        }
    }

    MemoryBlock& operator=(const MemoryBlock& other) {
        if (this != &other) {
            T* data = nullptr;
            if (other.m_size > 0 && other.m_data != nullptr) {
                // Allocate before releasing so a failed copy leaves *this intact.
                data = allocate(other.m_size);
                std::memcpy(data, other.m_data, sizeof(T) * other.m_size);
            }
            std::free(m_data);
            m_data = data;
            m_size = other.m_size;
        }
        return *this;
    }

    MemoryBlock(MemoryBlock&& other) noexcept : m_data(other.m_data), m_size(other.m_size) {
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

    T& operator[](size_t index) {
        assert(m_data != nullptr && "MemoryBlock: subscript on null data");
        return m_data[index];
    }
    const T& operator[](size_t index) const {
        assert(m_data != nullptr && "MemoryBlock: subscript on null data");
        return m_data[index];
    }

    T* data() { return m_data; }
    const T* data() const { return m_data; }
    size_t size() const { return m_size; }

    /// Resize the block, preserving the first min(old, new) elements. On
    /// allocation failure the block is left unchanged (old data and old size)
    /// and std::bad_alloc is thrown, so size() never claims more elements than
    /// data() actually holds.
    void resize(size_t size) {
        if (size == 0) {
            // malloc(0)/realloc(p, 0) are implementation-defined; release
            // explicitly so a zero-sized block is always the null block.
            std::free(m_data);
            m_data = nullptr;
            m_size = 0;
            return;
        }
        void* data = (m_data != nullptr) ? std::realloc(m_data, sizeof(T) * size)
                                         : std::malloc(sizeof(T) * size);
        if (data == nullptr) { throw std::bad_alloc(); }
        m_data = static_cast<T*>(data);
        m_size = size;
    }

    void clear() {
        if (m_data != nullptr) { std::memset(m_data, 0, sizeof(T) * m_size); }
    }

    /// Exchange storage with another block of the same size. Precondition:
    /// size() == other.size(); a mismatch asserts in debug and is a no-op in
    /// release.
    template <typename U = T>
        requires(std::is_trivially_copyable_v<U>)
    void swap_data(MemoryBlock& other) {
        if (this == &other) { return; }
        assert(m_size == other.m_size && "MemoryBlock: cannot swap data with different sizes");
        if (m_size == other.m_size) { std::swap(m_data, other.m_data); }
    }

    /// Exchange storage with a raw malloc'd pointer holding `size` elements.
    /// Precondition: size() == size; a mismatch asserts in debug and is a
    /// no-op in release.
    void swap_data(T*& data, size_t size) {
        assert(m_size == size && "MemoryBlock: cannot swap data with different sizes");
        if (m_size == size) { std::swap(m_data, data); }
    }

private:
    static T* allocate(std::size_t count) {
        void* data = std::malloc(sizeof(T) * count);
        if (data == nullptr) { throw std::bad_alloc(); }
        return static_cast<T*>(data);
    }

    T* m_data = nullptr;
    size_t m_size = 0;
};

}  // namespace thl::core
