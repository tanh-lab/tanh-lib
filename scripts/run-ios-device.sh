#!/bin/bash
# Run an iOS app bundle on a physical device and stream its output.
#
# Usage (standalone):
#   run-ios-device.sh <path-to.app> [extra args...]
#
# Usage (CTest emulator — set via CMAKE_CROSSCOMPILING_EMULATOR):
#   run-ios-device.sh <path-to-executable-inside-.app> [--gtest_filter=...]
#
# Accepts either a .app bundle directory or an executable inside one.
# When CTest + Xcode generator leaves a literal ${EFFECTIVE_PLATFORM_NAME}
# in the path, the script fixes it up automatically.
#
# Environment variables:
#   IOS_DEVICE_UDID  — target a specific device (default: first connected device)
set -euo pipefail

INPUT="$1"
shift

# ── Resolve .app bundle path ────────────────────────────────────────────────

if [[ -d "$INPUT" && "$INPUT" == *.app ]]; then
    APP_PATH="$INPUT"
else
    EXECUTABLE="$INPUT"

    # Fix paths where ${EFFECTIVE_PLATFORM_NAME} was not expanded by CTest.
    EXECUTABLE=$(echo "$EXECUTABLE" | sed 's|\${EFFECTIVE_PLATFORM_NAME}||g')

    if [ ! -f "$EXECUTABLE" ]; then
        for suffix in "-iphoneos" "-iphonesimulator"; do
            FIXED=$(echo "$EXECUTABLE" | sed "s|/Debug/|/Debug${suffix}/|;s|/Release/|/Release${suffix}/|;s|/RelWithDebInfo/|/RelWithDebInfo${suffix}/|;s|/MinSizeRel/|/MinSizeRel${suffix}/|")
            if [ -f "$FIXED" ]; then
                EXECUTABLE="$FIXED"
                break
            fi
        done
    fi

    if [ ! -f "$EXECUTABLE" ]; then
        echo "Error: Executable not found: $EXECUTABLE" >&2
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

# ── Find connected device ───────────────────────────────────────────────────

if [ -n "${IOS_DEVICE_UDID:-}" ]; then
    DEVICE_UDID="$IOS_DEVICE_UDID"
else
    DEVICE_UDID=$(xcrun devicectl list devices 2>/dev/null \
        | grep -i 'available' \
        | head -1 \
        | grep -oE '[0-9A-Fa-f-]{36}' \
        || true)

    if [ -z "$DEVICE_UDID" ]; then
        echo "Error: No connected iOS device found. Plug in a device or set IOS_DEVICE_UDID." >&2
        exit 1
    fi
fi

echo "Device: $DEVICE_UDID"
echo "App:    $APP_PATH"
echo "Bundle: $BUNDLE_ID"

# ── Terminate if already running ────────────────────────────────────────────

xcrun devicectl device process terminate --device "$DEVICE_UDID" "$BUNDLE_ID" 2>/dev/null || true

# ── Install and launch ──────────────────────────────────────────────────────

xcrun devicectl device install app --device "$DEVICE_UDID" "$APP_PATH"
xcrun devicectl device process launch --device "$DEVICE_UDID" --console "$BUNDLE_ID" "$@"
