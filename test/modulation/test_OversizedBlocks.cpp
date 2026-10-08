#include <gtest/gtest.h>
#include <tanh/modulation/ModulationMatrix.h>

#include "XYControllerTestHelpers.h"

using namespace thl::modulation;
using xy_test::FakeBackend;
using xy_test::k_sr;

namespace {
constexpr uint32_t k_bs = 256;  // prepared maximum
}  // namespace

// Engines split host blocks larger than the prepared size into prepared-size
// matrix blocks (docs/sphinx/transport.md, "Block size").
// The matrix's buffers hold the prepared maximum: a larger num_samples is a
// contract violation (assert), clamped in a release build.
TEST(OversizedBlocksDeathTest, MatrixRejectsBlocksAboveThePreparedSize) {
    FakeBackend backend;
    backend.add("p", 0.0f);
    ModulationMatrix matrix(backend);
    matrix.prepare(k_sr, k_bs);
    matrix.process(k_bs);
    // Under RTSan the assert's own message write is reported first (the matrix
    // runs as a non-blocking function), so either report marks the violation.
    EXPECT_DEBUG_DEATH(matrix.process(k_bs + 1),
                       "exceeds the prepared samples_per_block|RealtimeSanitizer");
}
