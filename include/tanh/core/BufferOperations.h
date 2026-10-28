// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/core/BufferView.h>

#include <algorithm>
#include <cassert>
#include <cstddef>

namespace thl::core {

/**
 * Operations on the samples of buffer views, sample by sample in every channel.
 *
 * @p destination and @p source must have the same number of channels and samples (asserted in
 * debug builds) and must not overlap.
 */
struct BufferOperations {
    BufferOperations() = delete;

    static void copy(BufferView destination, const ConstBufferView& source) {
        assert(same_dimensions(destination, source) &&
               "BufferOperations: cannot copy, views have different dimensions");
        for (size_t channel = 0; channel < destination.get_num_channels(); ++channel) {
            std::copy_n(source.get_read_pointer(channel),
                        destination.get_num_samples(),
                        destination.get_write_pointer(channel));
        }
    }

    static void add(BufferView destination, const ConstBufferView& source) {
        assert(same_dimensions(destination, source) &&
               "BufferOperations: cannot add, views have different dimensions");
        for (size_t channel = 0; channel < destination.get_num_channels(); ++channel) {
            float* output = destination.get_write_pointer(channel);
            const float* input = source.get_read_pointer(channel);
            for (size_t i = 0; i < destination.get_num_samples(); ++i) { output[i] += input[i]; }
        }
    }

private:
    static bool same_dimensions(const BufferView& destination, const ConstBufferView& source) {
        return destination.get_num_channels() == source.get_num_channels() &&
               destination.get_num_samples() == source.get_num_samples();
    }
};

}  // namespace thl::core
