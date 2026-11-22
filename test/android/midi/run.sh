#!/bin/bash
# Builds the Android MIDI test apps and runs them on the connected device or emulator.
#
# No MIDI hardware is needed: the test app brings two virtual MIDI devices (MIDI 1.0 and
# Universal MIDI Packets) and drives thl::midi::DeviceManager against them; a second app
# that is only a virtual device is installed and uninstalled to play a device being
# plugged in and pulled out. No Gradle either — the APKs are put together with the SDK's
# own tools.
#
# Needs: ANDROID_HOME (platform android-36, build-tools 36.0.0, platform-tools), an NDK
# (ANDROID_NDK_HOME, or the newest one under $ANDROID_HOME/ndk), a JDK 17, cmake, ninja,
# and one device in `adb devices`.
#
# Usage: run.sh [build-dir]
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="${1:-$ROOT/build/android-midi-test}"
ABI="${ANDROID_TEST_ABI:-arm64-v8a}"

SDK="${ANDROID_HOME:?set ANDROID_HOME}"
NDK="${ANDROID_NDK_HOME:-$(ls -d "$SDK"/ndk/* | sort -V | tail -1)}"
BUILD_TOOLS="$SDK/build-tools/36.0.0"
PLATFORM_JAR="$SDK/platforms/android-36/android.jar"
ADB="$SDK/platform-tools/adb"
PACKAGE=io.github.tanhlab.miditest

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# build_apk <name> <manifest> <res dir> <native lib or ""> <java files...>
build_apk() {
    local name="$1" manifest="$2" res="$3" lib="$4"
    shift 4
    local dir="$OUT/$name"
    rm -rf "$dir"
    mkdir -p "$dir/classes" "$dir/dex" "$dir/apk"

    javac -source 8 -target 8 -Xlint:-options -bootclasspath "$PLATFORM_JAR" \
        -d "$dir/classes" "$@"
    find "$dir/classes" -name '*.class' -print0 |
        xargs -0 "$BUILD_TOOLS/d8" --lib "$PLATFORM_JAR" --min-api 24 --output "$dir/dex"

    "$BUILD_TOOLS/aapt2" compile --dir "$res" -o "$dir/res.zip"
    "$BUILD_TOOLS/aapt2" link -o "$dir/unaligned.apk" -I "$PLATFORM_JAR" \
        --manifest "$manifest" "$dir/res.zip"
    cp "$dir/dex/classes.dex" "$dir/apk/"
    if [ -n "$lib" ]; then
        mkdir -p "$dir/apk/lib/$ABI"
        cp "$lib" "$dir/apk/lib/$ABI/"
    fi
    (cd "$dir/apk" && zip -q -r "$dir/unaligned.apk" .)
    "$BUILD_TOOLS/zipalign" -f 4 "$dir/unaligned.apk" "$dir/aligned.apk"
    "$BUILD_TOOLS/apksigner" sign --ks "$OUT/debug.keystore" --ks-pass pass:android \
        --key-pass pass:android --out "$OUT/$name.apk" "$dir/aligned.apk"
}

echo "== native library ($ABI)"
cmake -S "$HERE/native" -B "$OUT/native" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-24 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Debug > "$OUT/cmake.log" 2>&1 || { cat "$OUT/cmake.log"; exit 1; }
cmake --build "$OUT/native" --target miditest > "$OUT/build.log" 2>&1 ||
    { cat "$OUT/build.log"; exit 1; }

echo "== apps"
if [ ! -f "$OUT/debug.keystore" ]; then
    keytool -genkeypair -keystore "$OUT/debug.keystore" -storepass android -keypass android \
        -alias debug -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=tanh midi test" > /dev/null 2>&1
fi
# The library's own Java half is compiled in exactly as an app does it.
build_apk miditest "$HERE/AndroidManifest.xml" "$HERE/res" "$OUT/native/libmiditest.so" \
    "$ROOT"/android/java/io/github/tanhlab/midi/*.java \
    "$HERE"/java/io/github/tanhlab/miditest/*.java
build_apk plug "$HERE/plug/AndroidManifest.xml" "$HERE/plug/res" "" \
    "$HERE"/plug/java/io/github/tanhlab/miditest/plug/*.java

cleanup() {
    "$ADB" uninstall "$PACKAGE.plug" > /dev/null 2>&1 || true
    "$ADB" uninstall "$PACKAGE" > /dev/null 2>&1 || true
}
trap cleanup EXIT
cleanup

echo "== devices that are present"
"$ADB" install -r "$OUT/miditest.apk" > /dev/null
MAIN="$("$ADB" shell am instrument -w "$PACKAGE/.MidiTest")"
echo "$MAIN"

echo "== a device that comes and goes"
"$ADB" shell am instrument -w -e mode hotplug "$PACKAGE/.MidiTest" > "$OUT/hotplug.txt" &
INSTRUMENT=$!
sleep 4                                   # the test is waiting for a device now
"$ADB" install -r "$OUT/plug.apk" > /dev/null
sleep 8                                   # listed, opened, heard
"$ADB" uninstall "$PACKAGE.plug" > /dev/null
wait "$INSTRUMENT"
cat "$OUT/hotplug.txt"

echo "$MAIN" | grep -q "^OK: " && grep -q "^OK: " "$OUT/hotplug.txt"
