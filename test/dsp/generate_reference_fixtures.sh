#!/usr/bin/env bash
#
# Generate Rings reference fixtures from the original Mutable Instruments code.
#
# This script clones the mutable-instrument-api repo, builds the reference
# generators, and runs them to produce fixture files. The tanh-lib Rings
# tests then compare the refactored code against these fixtures.
#
# Usage:
#   ./test/dsp/generate_reference_fixtures.sh
#
# The script places fixtures in test/dsp/fixtures/ which are picked up
# by CMake (tanh_add_binary_data) when building the test target.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIXTURES_DIR="${SCRIPT_DIR}/fixtures"
REPO_URL="git@github.com:tanh-lab/mutable-instrument-api.git"
REPO_BRANCH="main"

WORK_DIR="${SCRIPT_DIR}/../../build-reference-gen"

echo "=== Rings Reference Fixture Generator ==="
echo ""
echo "Fixtures dir : ${FIXTURES_DIR}"
echo "Work dir     : ${WORK_DIR}"
echo ""

# Clean previous run
rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"

cleanup() {
    echo "Cleaning up work directory..."
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

# -- 1. Clone mutable-instrument-api ----------------------------------------

echo "Cloning ${REPO_URL} (branch: ${REPO_BRANCH})..."
git clone --depth 1 --recurse-submodules --branch "${REPO_BRANCH}" "${REPO_URL}" "${WORK_DIR}/mutable-instrument-api"
echo ""

# -- 2. Build the reference generators --------------------------------------

MI_DIR="${WORK_DIR}/mutable-instrument-api/mi-eurorack-api"
BUILD_DIR="${WORK_DIR}/build"

echo "Configuring CMake..."
cmake -S "${MI_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMI_BUILD_TESTS=ON
echo ""

NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

echo "Building generators..."
cmake --build "${BUILD_DIR}" \
    --target mi-rings-generate-reference \
    --target mi-rings-generate-wrapper-reference \
    -j"${NPROC}"
echo ""

# -- 3. Generate fixtures ---------------------------------------------------

# Generators write to their compiled-in FIXTURE_OUTPUT_DIR
# (mi-eurorack-api/tests/fixtures/rings inside the clone).
GEN_FIXTURES_DIR="${MI_DIR}/tests/fixtures/rings"

echo "Generating raw DSP reference fixtures..."
"${BUILD_DIR}/mi-rings-generate-reference"
echo ""

echo "Generating wrapper reference fixtures..."
"${BUILD_DIR}/mi-rings-generate-wrapper-reference"
echo ""

# -- 4. Copy fixtures to tanh-lib -------------------------------------------

mkdir -p "${FIXTURES_DIR}"
cp "${GEN_FIXTURES_DIR}"/*.bin "${FIXTURES_DIR}/"

echo ""
echo "=== Done ==="
echo ""
echo "Fixtures written to: ${FIXTURES_DIR}"
ls -lh "${FIXTURES_DIR}"/*.bin
echo ""
echo "Now rebuild and run the tests:"
echo "  cmake --build <build-dir> --target test_dsp"
echo "  ./<build-dir>/test/dsp/test_dsp"
