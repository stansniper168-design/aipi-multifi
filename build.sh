#!/usr/bin/env bash
# Build the AIPI Lite multi-Wi-Fi firmware from stock upstream SDK.
# No source changes — the multi-Wi-Fi feature (wifi_known, up to 8 saved
# networks) already exists upstream; this just builds a newer image than
# the official AIPI 1.0.4 release. Linux (or WSL2). ~10 min after toolchain.
set -euo pipefail

SDK_REPO="https://github.com/facebookincubator/muse-gadget-sdk.git"
SDK_COMMIT="e17081a"          # verified 2026-11-23
IDF_TAG="v6.0.3"              # ESP-IDF version the SDK requires, exactly
IDF_DIR="${HOME}/esp/esp-idf-v6"
SDK_DIR="${SDK_DIR:-$PWD/muse-gadget-sdk}"

# --- SDK checkout (stock, pinned commit) ---
if [ ! -d "$SDK_DIR/.git" ]; then
  git clone "$SDK_REPO" "$SDK_DIR"
fi
git -C "$SDK_DIR" checkout "$SDK_COMMIT"
git -C "$SDK_DIR" status --porcelain | grep -q . && {
  echo "WARNING: SDK checkout has local changes — build will not be stock."
}

# --- ESP-IDF v6.0.3 toolchain ---
if [ ! -d "$IDF_DIR" ]; then
  git clone -b "$IDF_TAG" --depth 1 --recursive \
    https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi
"$IDF_DIR/install.sh" esp32s3
# shellcheck disable=SC1091
. "$IDF_DIR/export.sh"

# --- Build the aipi profile ---
cd "$SDK_DIR/esp32"
# idf.py v6.0 has no -j flag; limit parallelism via CMake instead.
CMAKE_BUILD_PARALLEL_LEVEL=2 idf.py -B build-muse-aipi -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-muse-aipi/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-aipi" \
  build

# --- Verify multi-Wi-Fi is in the binary ---
ELF="build-muse-aipi/muse-gadget.elf"
for sym in join_saved_networks wifi_known_load wifi_known_remember; do
  xtensa-esp32s3-elf-nm "$ELF" | grep -q "$sym" || { echo "MISSING symbol: $sym"; exit 1; }
done
echo "OK: multi-Wi-Fi symbols present."
echo "Binaries: build-muse-aipi/muse-gadget.bin + flasher_args.json"
echo "Flash: copy flash.bat + flasher_args.json into build-muse-aipi/ and run flash.bat COMx (esptool, never erase-flash)."
