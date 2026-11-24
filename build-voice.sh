#!/usr/bin/env bash
# Build the AIPI Lite Groq voice firmware: spoken replies via Groq's Orpheus
# TTS, no OpenAI anywhere. Stock upstream SDK (HEAD e17081a, ESP-IDF v6.0.3,
# `aipi` profile) + the voice patch in voice/voice.patch + the new files in
# voice/. Linux (or WSL2). ~10 min after toolchain.
set -euo pipefail

SDK_REPO="https://github.com/facebookincubator/muse-gadget-sdk.git"
SDK_COMMIT="e17081a"          # verified 2026-11-23
IDF_TAG="v6.0.3"              # ESP-IDF version the SDK requires, exactly
IDF_DIR="${HOME}/esp/esp-idf-v6"
SDK_DIR="${SDK_DIR:-$PWD/muse-gadget-sdk}"
# voice/ lives next to this script (the repo checkout).
PATCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/voice" && pwd)"

# --- SDK checkout (stock, pinned commit) ---
if [ ! -d "$SDK_DIR/.git" ]; then
  git clone "$SDK_REPO" "$SDK_DIR"
fi
git -C "$SDK_DIR" checkout "$SDK_COMMIT"
git -C "$SDK_DIR" status --porcelain | grep -q . && {
  echo "WARNING: SDK checkout has local changes — build will not be stock + voice patch."
}

# --- Apply the voice patch + copy the new voice files ---
git -C "$SDK_DIR" apply --check "$PATCH_DIR/voice.patch"
git -C "$SDK_DIR" apply "$PATCH_DIR/voice.patch"
cp "$PATCH_DIR/muse_tts_groq.c" "$PATCH_DIR/muse_tts_groq.h" \
  "$SDK_DIR/esp32/components/muse/"
cp "$PATCH_DIR/tests/test_muse_tts_groq.py" \
  "$PATCH_DIR/tests/test_muse_tts_groq_harness.c" \
  "$SDK_DIR/esp32/tests/"
echo "OK: voice patch applied, new files copied."

# --- ESP-IDF v6.0.3 toolchain ---
if [ ! -d "$IDF_DIR" ]; then
  git clone -b "$IDF_TAG" --depth 1 --recursive \
    https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi
"$IDF_DIR/install.sh" esp32s3
# shellcheck disable=SC1091
. "$IDF_DIR/export.sh"

# --- Build the aipi profile ---
# CONFIG_MUSE_TTS_GROQ defaults to y (see voice/voice.patch, Kconfig), so
# the stock aipi sdkconfig picks the voice code up with no extra flags.
cd "$SDK_DIR/esp32"
# idf.py v6.0 has no -j flag; limit parallelism via CMake instead.
CMAKE_BUILD_PARALLEL_LEVEL=2 idf.py -B build-muse-aipi -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-aipi/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-aipi" \
  build

# --- Verify multi-Wi-Fi + voice are in the binary ---
ELF="build-muse-aipi/muse-gadget.elf"
for sym in join_saved_networks wifi_known_load wifi_known_remember; do
  xtensa-esp32s3-elf-nm "$ELF" | grep -q "$sym" || { echo "MISSING symbol: $sym"; exit 1; }
done
for sym in muse_tts_groq_start muse_chat_tts_feed muse_chat_tts_end decode_wav; do
  xtensa-esp32s3-elf-nm "$ELF" | grep -q "$sym" || { echo "MISSING symbol: $sym"; exit 1; }
done
grep -q "CONFIG_MUSE_TTS_GROQ=y" build-muse-aipi/sdkconfig || {
  echo "CONFIG_MUSE_TTS_GROQ is not y in the generated sdkconfig"; exit 1; }
echo "OK: multi-Wi-Fi + Groq voice symbols present."
echo "Binaries: build-muse-aipi/muse-gadget.bin + flasher_args.json"
echo "Flash: copy flash.bat + flasher_args.json into build-muse-aipi/ and run flash.bat COMx (esptool, never erase-flash)."
echo "Then configure your own Groq key over the serial console (115200 baud):"
echo '  >voice={"url":"https://api.groq.com/openai/v1/audio/speech","model":"canopylabs/orpheus-v1-english","voice":"troy","key":"gsk_..."}'
