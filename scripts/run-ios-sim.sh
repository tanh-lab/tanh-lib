#!/bin/bash
# Run an iOS test bundle on the simulator and stream its output.
#
# Usage (standalone):
#   run-ios-test.sh <path-to.app> [extra args...]
#
# Usage (CTest emulator — set via CMAKE_CROSSCOMPILING_EMULATOR):
#   run-ios-test.sh <path-to-executable-inside-.app> [--gtest_filter=...]
#
# Accepts either a .app bundle directory or an executable inside one.
# When CTest + Xcode generator leaves a literal ${EFFECTIVE_PLATFORM_NAME}
# in the path, the script fixes it up automatically.
set -euo pipefail

INPUT="$1"
shift

# ── Resolve .app bundle path ────────────────────────────────────────────────

if [[ -d "$INPUT" && "$INPUT" == *.app ]]; then
    # Standalone mode: given a .app directory directly
    APP_PATH="$INPUT"
else
    # CTest emulator mode: given the executable inside the .app
    EXECUTABLE="$INPUT"

    # Fix paths where ${EFFECTIVE_PLATFORM_NAME} was not expanded by CTest.
    # CTest uses exec() (not a shell), so the Xcode variable arrives as the
    # literal text "${EFFECTIVE_PLATFORM_NAME}".  Strip it out first, then
    # try appending platform suffixes.
    # e.g.  .../Debug${EFFECTIVE_PLATFORM_NAME}/...  →  .../Debug/...  →  .../Debug-iphonesimulator/...
    EXECUTABLE=$(echo "$EXECUTABLE" | sed 's|\${EFFECTIVE_PLATFORM_NAME}||g')

    if [ ! -f "$EXECUTABLE" ]; then
        for suffix in "-iphonesimulator" "-iphoneos"; do
            FIXED=$(echo "$EXECUTABLE" | sed "s|/Debug/|/Debug${suffix}/|;s|/Release/|/Release${suffix}/|;s|/RelWithDebInfo/|/RelWithDebInfo${suffix}/|;s|/MinSizeRel/|/MinSizeRel${suffix}/|")
            if [ -f "$FIXED" ]; then
                EXECUTABLE="$FIXED"
                break
            fi
        done
    fi

    if [ ! -f "$EXECUTABLE" ]; then
        echo "Error: Test executable not found: $EXECUTABLE" >&2
        exit 1
    fi

    APP_PATH=$(dirname "$EXECUTABLE")
    if [[ "$APP_PATH" != *.app ]]; then
        echo "Error: Expected executable inside an .app bundle, got: $EXECUTABLE" >&2
        exit 1
    fi
fi

# ── Extract bundle identifier ───────────────────────────────────────────────

BUNDLE_ID=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "${APP_PATH}/Info.plist" 2>/dev/null || true)
if [ -z "$BUNDLE_ID" ]; then
    APP_NAME=$(basename "$APP_PATH" .app)
    BUNDLE_ID="com.tanh.${APP_NAME}"
fi

# ── Find and boot simulator ─────────────────────────────────────────────────

DEVICE_FAMILY="${IOS_TEST_DEVICE_FAMILY:-iPhone}"

SIMULATOR_INFO=$(xcrun simctl list devices available | grep -m 1 "${DEVICE_FAMILY}" | head -n 1)
if [ -z "$SIMULATOR_INFO" ]; then
    echo "Error: No ${DEVICE_FAMILY} simulators found" >&2
    exit 1
fi
SIMULATOR_UDID=$(echo "$SIMULATOR_INFO" | grep -E -o '([A-F0-9-]{36})')

xcrun simctl boot "$SIMULATOR_UDID" 2>/dev/null || true

# ── Terminate if already running ────────────────────────────────────────────

xcrun simctl terminate "$SIMULATOR_UDID" "$BUNDLE_ID" 2>/dev/null || true

# ── Install and launch ──────────────────────────────────────────────────────

xcrun simctl install "$SIMULATOR_UDID" "$APP_PATH"
xcrun simctl launch --console-pty "$SIMULATOR_UDID" "$BUNDLE_ID" "$@"
