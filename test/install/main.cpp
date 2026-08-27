// SPDX-License-Identifier: Apache-2.0
// Touches every core header an installed consumer is expected to have and
// links against the compiled part of tanh::Core (Logger, version).
#include <tanh/core.h>
#include <tanh/core/Buffer.h>
#include <tanh/core/Logger.h>
#include <tanh/core/MemoryBlock.h>
#include <tanh/core/RingBuffer.h>
#include <tanh/core/threading/LockFreeQueue.h>

#include <cstdio>

int main() {
    thl::core::RingBuffer<int> ring;
    ring.initialise_with_positions(1, 4);
    for (int i = 0; i < 6; ++i) { ring.push_sample(0, i); }

    thl::core::Buffer<float> buffer(2, 16);
    buffer.set_sample(1, 3, 0.5F);

    thl::Logger::info("install-consumer", "tanh::Core linked from the install tree");
    std::printf("version=%s oldest=%d available=%zu sample=%g\n",
                thl::core::get_version().c_str(),
                ring.pop_sample(0),
                ring.get_available_samples(0),
                static_cast<double>(buffer.get_sample(1, 3)));

    return (ring.get_available_samples(0) == 3 && buffer.get_num_samples() == 16) ? 0 : 1;
}
