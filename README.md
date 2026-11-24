# AIPI Lite multi-Wi-Fi — stock SDK build, 8 saved networks

The [AIPI Lite](https://muse.aipi.com/) (X-Origin) is a little ESP32-S3 desk
gadget that runs Muse. Official AIPI firmware 1.0.4 only remembers **one**
Wi-Fi network, so the box goes dark the moment you leave your home network —
unless you re-pair it every time.

The upstream [muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
already has multi-Wi-Fi: `wifi_known` stores up to **8 saved networks** and
`join_saved_networks()` tries them in turn at boot and on disconnect. This
repo is a **stock build of that SDK** (HEAD `e17081a`, ESP-IDF v6.0.3, `aipi`
profile) with **zero source changes** — just a newer build than the one AIPI
ships, so the feature is switched on.

## Verified working

Tested 2026-11-23 on a real AIPI Lite (device D4154C), flashed with esptool:

- Box saved two networks: RV-park Wi-Fi + phone hotspot (2.4 GHz).
- Hotspot turned off → the box **hopped back to the park Wi-Fi on its own**
  and stayed online. Roam takes roughly 5–30 seconds.
- Flashing does not touch NVS, so pairing, the SDK token, and the saved
  park Wi-Fi all survived.

## What's here

- `build.sh` — repeatable build from stock upstream (Linux; ~10 minutes once
  the toolchain is installed). No SDK token needed at build time — the token
  is read from the device's NVS, and the build warns "No SDK token" on
  purpose.
- `flash.bat` + `flasher_args.json` — the build's own flash manifest
  (bootloader 0x0, partition table 0x10000, otadata 0x1d000, app 0x20000).
  After building, copy these two files into `build-muse-aipi/` next to the
  `.bin` files and run them there. Flash with esptool on Windows or Linux;
  a USB-C **data** cable is required (a charge-only cable fails with "No
  compatible devices found").
- Prebuilt binaries are **not** in this repo. Build them with `build.sh` —
  the SHA-256 hashes below let you confirm your build matches the verified
  one byte-for-byte.

| File | Address | Size | SHA-256 |
|---|---|---|---|
| bootloader.bin | 0x0 | 22,752 | `3caabad8bb9fd5869cca48f70ed2ca2134e6136eaa70dff1b8033f62c4f5abdc` |
| partition-table.bin | 0x10000 | 3,072 | `f1eed33f4d8c528ce6085b46abfeb3a176beefe71601330a81c1cc15fc58e08f` |
| ota_data_initial.bin | 0x1d000 | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| muse-gadget.bin | 0x20000 | 2,232,320 | `6a66ba5b8b16d2fc0b332dcadedff2c18d1c9c33cecf135ee1fea152ea78ced1` |

## Flash it

1. Python 3.12 + `pip install esptool`.
2. Plug the box in with the USB-C data cable. Find the port: Device
   Manager → Ports → "USB JTAG/serial debug unit (COMx)".
3. Copy `flash.bat` into `build-muse-aipi/` (next to the `.bin` files) and
   run `flash.bat COMx` from there (or the raw esptool command in that
   file).
4. Wait for "Hash of data verified" on all four files and the hard reset.
   Unplug — the box boots, rejoins its saved Wi-Fi, and shows the face.

**NEVER run `erase-flash`** — it wipes pairing, the SDK token, and saved
Wi-Fi. This flash does not touch NVS (0x11000).

**Before you flash:** make sure you have the stock backup the official
installer makes (`aipi-backup-D4154C.bin`, 16 MB) stored somewhere safe.
That plus the way-back below is your recovery.

## Adding the second network — the part that took figuring out

The app (17.0.0.20.160) does **not** show a saved-networks list in the
gadget settings, and the box's own Settings → Wi-Fi row is a toggle only —
no list. The setup flow is the only way in, and it has a catch:

1. **A paired box never shows up in the app's Add Device scan.** The
   firmware only advertises for setup while the device is *unpaired* — the
   "Phone setup" toggle just turns Bluetooth on, it doesn't make a paired
   box discoverable.
2. So: in the Muse app, **Disconnect the gadget FIRST, then Add Device**.
   The setup flow then offers the saved Wi-Fi networks — pick the second
   one (your phone hotspot; it must be on and broadcasting on **2.4 GHz**,
   the box can't see 5 GHz). Adding a network does **not** wipe the first:
   an app-side unpair keeps saved Wi-Fi.
3. **Do NOT use the box's own menu "Reset pairing"** — that one forgets
   Wi-Fi too. The disconnect/remove must be done in the app.
4. Test it: turn the hotspot off and watch the box hop back to the first
   network on its own. If it answers, the roam works.

## Way back

Re-run [muse.aipi.com/setup.html](https://muse.aipi.com/setup.html) — it
re-installs official Muse 1.0.4, and NVS (pairing, token, saved Wi-Fi)
survives. Or restore the stock backup file.

## Risks

- This is a dev build (SDK version 999.0.0), not an AIPI-blessed release.
  A later official update will overwrite it with stock firmware — if stock
  still lacks multi-Wi-Fi then, the box drops to single-network until you
  re-flash this build.
- Bricking risk is low: no Secure Boot or flash encryption on the AIPI
  Lite, the ROM bootloader always answers, and a bad OTA rolls back on its
  own. Still — hackers' project, proceed at your own risk, and keep the
  stock backup safe.

## Credit

Upstream code is Meta's
[facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
(Apache 2.0). The disconnect-first procedure was found by testing on a real
box, 2026-11-23.
