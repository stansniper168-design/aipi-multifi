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
- `build-voice.sh` + `voice/` — the same build **plus spoken replies** via
  Groq TTS (see [Voice (Groq TTS, no OpenAI)](#voice-groq-tts-no-openai)
  below). No OpenAI anywhere.
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

## Voice (Groq TTS, no OpenAI)

The open SDK's TTS is a stub, so spoken replies needed a real engine. This
build speaks through **Groq's Orpheus TTS**
(`canopylabs/orpheus-v1-english`) via Groq's OpenAI-compatible endpoint
(`https://api.groq.com/openai/v1/audio/speech`). **No OpenAI anywhere.**
Orpheus is WAV-only with a 200-character-per-request cap, so replies are
chunked at sentence boundaries (up to 6 requests for a long reply) and
played as 16 kHz mono audio. Default voice is `troy`. Free tier is ~100
requests/day.

**Verified working 2026-11-24 on a real AIPI Lite (device D4154C):** first
test turn "Say hello to me" → Groq answered HTTP 200, reply audio 24 kHz
WAV, **3.44 seconds of speech played**. Laptop-side check
(`voice/groq_test.py`) from the same Wi-Fi: HTTP 200, `audio/wav`, 76,870
bytes.

**Two blockers found and fixed:**

1. **Cloudflare 1010.** Groq's Cloudflare returns HTTP 403 code 1010 for
   non-browser User-Agents, and the ESP32 HTTP client's default
   (`"ESP32 HTTP Client/1.0"`) was blocked at the edge before any HTTP
   response arrived. Fix: a browser-like User-Agent header
   (`MUSE_TTS_GROQ_USER_AGENT`, Chrome 120).
2. **Orpheus terms acceptance.** Groq returns HTTP 400
   `model_terms_required` until the org admin accepts the Orpheus model
   terms in the Groq console:
   https://console.groq.com/playground?model=canopylabs%2Forpheus-v1-english
   — a console step, not firmware. (A bad key would 401, not 400, so this
   also proves the key is valid.)

The first build also had a diagnostic gap: the fetch died before any HTTP
response and every pre-response exit was a silent `goto out`. The fix adds
WARN logging (15 new `ESP_LOGW` lines) naming the stage and error on every
failure exit, so the serial log now says exactly where a fetch died.

**Build it:** `build-voice.sh` — same pinned stock SDK (`e17081a`,
ESP-IDF v6.0.3, `aipi` profile) plus the voice patch;
`CONFIG_MUSE_TTS_GROQ=y` (default in the patch's Kconfig).
**Flash it:** same 4-file esptool procedure as above; never `erase-flash`
(NVS with token, pairing, Wi-Fi, and voice settings survives a normal
flash). **Configure it:** over the serial console (115200 baud) with your
own free Groq API key — the key is entered by you and never goes in this
repo:

```
>voice={"url":"https://api.groq.com/openai/v1/audio/speech","model":"canopylabs/orpheus-v1-english","voice":"troy","key":"gsk_..."}
```

The box answers `@voice saved`. A bare `>voice=` prints URL, model, voice,
and key-set status — **never the key**.

Verified voice firmware hashes (build 2026-11-24):

| File | Address | Size | SHA-256 |
|---|---|---|---|
| bootloader.bin | 0x0 | 22,752 | `4d65f4fb2782be4fb34f917e3dd2e97e500b9defdfa83b45a754d10db4c5cdf1` |
| partition-table.bin | 0x10000 | 3,072 | `f1eed33f4d8c528ce6085b46abfeb3a176beefe71601330a81c1cc15fc58e08f` |
| ota_data_initial.bin | 0x1d000 | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| muse-gadget.bin | 0x20000 | 2,232,320 | `c2d65bd15241585f758e54d901086c8831012764a1e200fc1c62b0c7b8e877d9` |

Zip SHA-256: `38bd15fadc30cfe0afc4652cf18dd5f3af17b44f7ac9a48c4335be962cefa58f`
(1,489,004 bytes). Prebuilt binaries are **not** in this repo — build them
with `build-voice.sh`. Note: ESP-IDF embeds the compile date/time in the
app binary, so a fresh build differs from these hashes but is functionally
identical (same sources, same sdkconfig — verified by rebuilding from
`build-voice.sh` on 2026-11-24).

**Limits:** Orpheus is WAV-only, 200 chars/request, ~100 requests/day on
the free tier; a failed fetch fails soft to silent text. Groq model churn
is the biggest non-technical risk (Groq already killed playai-tts) — if
Orpheus is retired, change the model string with `>voice=`, no reflash
needed. One global voice, no per-message selection yet. Way back:
re-flash the multi-Wi-Fi-only firmware, or re-run
`muse.aipi.com/setup.html` for official 1.0.4.

Full documentation: `voice/VOICE_BUILD_REPORT.md` and
`voice/VOICE_FIX_REPORT.md`. Laptop-side helpers (`voice/groq_test.py`,
`voice/send_voice.py`, `voice/send_raw.py`) read the key from a local file
and never print it.

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
