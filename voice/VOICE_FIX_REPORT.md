# AIPI Lite Groq voice — fix report (User-Agent + failure logging)

Built 2026-11-24 ~04:12 UTC (Nov 23 ~22:12 CST) from facebookincubator/muse-gadget-sdk,
HEAD `e17081a`, ESP-IDF v6.0.3, `aipi` profile. This is the voice build from
VOICE_BUILD_REPORT.md **plus this fix only** — no other behavior changed.

## Status: BUILD SUCCEEDED — firmware ready, NOT flashed

`BUILD_EXIT=0`. Incremental rebuild in the existing `build-muse-aipi/` (the
first attempt at this fix was killed ~2.5 min in by a runtime restart before
it wrote anything; the re-run completed cleanly in ~9 min with
`CMAKE_BUILD_PARALLEL_LEVEL=2`). One expected warning: **"No SDK token"** —
the build is token-less, exactly like the multi-Wi-Fi and first voice builds;
the token is read from the device's NVS at runtime. No token value was asked
for, used, or seen. No credentials were used anywhere in this work, and Groq
was never contacted.

- App version: **999.0.0** (dev build, same as before).
- `muse-gadget.bin` is 0x221000 bytes (2,232,320); **46.8% free** in the 4 MB ota_0 slot.
- `CONFIG_MUSE_TTS_GROQ=y` confirmed in the generated sdkconfig.
- Work happened only in `~/workspace/aipi-voice-build/`; the known-good
  multi-Wi-Fi tree at `~/workspace/aipi-multifi-scope/` was never touched
  (git status there shows only its own pre-existing untracked build artifacts,
  no modified tracked files).

## Why: the diagnosis, and what verified it

On the first voice firmware, a real box-mic turn logged `speaking message ...
(12 chars)` (fetch task started), then `reply audio after 9.38s` and PCM
progress to 0.76s, but **no `Groq TTS answered HTTP` line anywhere**. That
WARN log prints on any HTTP response, so the fetch died *before* receiving
headers — at transport creation, init, open, write, or fetch_headers — and
every one of those exits was a silent `goto out` with no log. The box
chirped (reply arrived) and showed text, but no speech.

A laptop test from the same Wi-Fi (script `groq_test.py`, key read from the
laptop file, never printed) found two blockers:

1. **Cloudflare 1010.** With a Python User-Agent, Groq's Cloudflare returns
   HTTP 403 error code 1010 (bad browser). With a browser User-Agent,
   Cloudflare lets it through. The firmware sent the ESP32 HTTP client's
   default User-Agent (`"ESP32 HTTP Client/1.0"`), so every fetch was 403'd
   at the edge — before any HTTP response the box could log.
2. **Orpheus terms acceptance.** With a browser UA, Groq returns HTTP 400
   `model_terms_required` — the Orpheus model needs its terms accepted in the
   Groq console. That is a console step for Mike, not a firmware fix. The
   key itself is valid (a bad key would 401, not 400).

Nothing in the code contradicted the diagnosis:

- `esp_http_client` sets the default User-Agent (`"ESP32 HTTP Client/1.0"`,
  `esp_http_client.c:160`) at init; `esp_http_client_set_header` goes
  through `http_header_set`, which **replaces** the value when the key
  already exists (`lib/http_header.c`) — so setting a browser UA after init
  is a real override, not a duplicate header.
- Success is already observable without new logs: `muse_chat_session.cpp`
  logs `speaking message` (line 2207) and `reply audio: %u Hz, %u ch, WAV`
  (line 2383) when the WAV header arrives. The missing pair (no `Groq TTS
  answered HTTP`, no `reply audio`) is exactly "died before headers".

## What changed

Only `esp32/components/muse/muse_tts_groq.{c,h}` and the host harness
`esp32/tests/test_muse_tts_groq_harness.c`. Endpoint, model, voice defaults,
chunking, WAV handling, fail-soft-to-text behavior and NVS keys are
identical.

### 1. Browser-like User-Agent (`muse_tts_groq.h`, `muse_tts_groq.c`)

New named constant, so it is easy to change:

```c
#define MUSE_TTS_GROQ_USER_AGENT \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
```

Set in `tts_fetch_chunk`, next to the Authorization/Content-Type headers:

```c
esp_http_client_set_header(c, "User-Agent", MUSE_TTS_GROQ_USER_AGENT);
```

### 2. WARN logging on every silent failure exit (`muse_tts_groq.c`)

15 new `ESP_LOGW` lines (18 `Groq TTS` log lines total in the file, up from
3). No key and no request body is ever logged. Each names the stage and the
error code/value:

| Exit | Log line |
|---|---|
| Request-body build fails (OOM) | `Groq TTS request body build failed (OOM)` |
| Proxy transport creation fails | `Groq TTS proxy transport creation failed` |
| Custom transport unavailable (build without it) | `Groq TTS custom transport unavailable in this build` |
| `esp_http_client_init` returns NULL | `Groq TTS http client init failed` |
| Auth-header alloc fails (OOM) | `Groq TTS auth header alloc failed (OOM)` |
| `esp_http_client_open` fails | `Groq TTS http open failed: %s (%d)` (esp_err_t) |
| `esp_http_client_write` fails | `Groq TTS http write failed: %d of %d bytes` |
| `esp_http_client_fetch_headers` fails | `Groq TTS fetch headers failed: %s (%d)` (esp_err_t) |
| Read-buffer alloc fails (OOM) | `Groq TTS read buffer alloc failed (OOM)` |
| Per-chunk deadline expires | `Groq TTS chunk timed out after %d ms` |
| `esp_http_client_read` fails | `Groq TTS http read failed: %d` |
| Chunking yields 0 or >8 chunks (`tts_fetch_task`) | `Groq TTS chunking failed: %u chunks` |
| Job alloc fails in `muse_tts_groq_start` | `Groq TTS job alloc failed (OOM)` |
| Text copy fails in `muse_tts_groq_start` | `Groq TTS text copy failed (OOM)` |
| Fetch task fails to start | `Groq TTS fetch task failed to start: %d` |

Deliberately **not** logged: `muse_chat_tts_feed` returning < 0 (turn moved
on — the normal interruption path, not a fetch failure; logging it at WARN
would be noise on every interrupted reply). Pre-existing logs kept as-is:
`Groq TTS answered HTTP %d`, `Groq TTS reply is not a PCM WAV`,
`Groq TTS chunk changed format mid-reply`.

### 3. Host test for the User-Agent

The header is set in the ESP-IDF-only fetch task, so it is not
host-testable through the body builder. Instead the harness now checks the
exact string the firmware will send (`test_user_agent` in
`test_muse_tts_groq_harness.c`): it starts with `Mozilla/5.0`, contains
`Chrome/`, and contains no `ESP32`. The actual header emission is verified
by the UA string's presence in the ELF (below).

## Host test results

| Suite | Result |
|---|---|
| `tests/test_muse_tts_groq.py` | **PASS** — harness incl. the new `test_user_agent` case (chunker, WAV parser, body, `>voice=` parser, UA string). ASan+UBSan, `-Wall -Wextra -Werror`. |
| `tests/test_muse_chat_session.py` (full) | **48 tests PASS** — unchanged from the first voice build. |

## Artifacts (in `firmware-aipi-voice/`)

| File | Address | Size | SHA-256 |
|---|---|---|---|
| bootloader.bin | 0x0 | 22,752 | `4d65f4fb2782be4fb34f917e3dd2e97e500b9defdfa83b45a754d10db4c5cdf1` |
| partition-table.bin | 0x10000 | 3,072 | `f1eed33f4d8c528ce6085b46abfeb3a176beefe71601330a81c1cc15fc58e08f` |
| ota_data_initial.bin | 0x1d000 | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| muse-gadget.bin | 0x20000 | 2,232,320 | `c2d65bd15241585f758e54d901086c8831012764a1e200fc1c62b0c7b8e877d9` |

Zip: `firmware-aipi-voice.zip` (1,489,004 bytes, `38bd15fadc30cfe0afc4652cf18dd5f3af17b44f7ac9a48c4335be962cefa58f`)
— the folder plus README.md and flash.bat (README updated with the new
app hash and a fix note). Only `muse-gadget.bin` changed vs the first voice
build; bootloader, partition table and ota_data_initial hashes are identical.

## Verification — the fix is in the binary

- UA string present in `muse-gadget.elf` (verbatim `Mozilla/5.0 ... Chrome/120.0.0.0 Safari/537.36`).
- All 15 new WARN strings present in the ELF (`Groq TTS http open failed`,
  `Groq TTS fetch headers failed`, `Groq TTS http write failed`,
  `Groq TTS http read failed`, `Groq TTS proxy transport creation failed`,
  `Groq TTS http client init failed`, `Groq TTS chunk timed out`,
  `Groq TTS fetch task failed to start`, plus the OOM/chunking ones).
- Voice symbols present (`muse_tts_groq_start`, `muse_chat_tts_feed`,
  `muse_chat_tts_end`, `decode_wav`).
- `CONFIG_MUSE_TTS_GROQ=y` in `build-muse-aipi/sdkconfig`.

## Flash procedure (Windows laptop, esptool.py — same as the voice build)

**Do NOT flash without Mike present. Flash/test is a later step with him.**

```bat
python -m esptool --chip esp32s3 -p COM3 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size keep --flash-freq 80m 0x0 bootloader.bin 0x10000 partition-table.bin 0x1d000 ota_data_initial.bin 0x20000 muse-gadget.bin
```

Or `flash.bat COM3` from the `firmware-aipi-voice/` folder. **Never
`erase-flash`**: it wipes pairing, the SDK token, saved Wi-Fi, and the voice
settings. A normal flash preserves NVS, so the token, pairing, both saved
Wi-Fi networks, and the voice settings survive. The voice settings already
on the box (`>voice=` readback: configured true) need no re-entry.

## What is still needed before/at flash test

(a) **Orpheus terms acceptance in the Groq console — Mike's step, separate
from firmware.** Until the terms are accepted, Groq answers HTTP 400
`model_terms_required` and replies stay silent text. The new logging will
show it plainly as `Groq TTS answered HTTP 400` in the serial log.

(b) **Flash + test with Mike present** (box on the laptop via the USB-C data
cable, COM3 last time). Watch the serial log for a spoken reply: expect
`speaking message ...`, then `Groq TTS answered HTTP 200`, then
`reply audio: %u Hz, %u ch, WAV`, then audible speech. If it still fails,
the log will now name the stage (open/write/fetch-headers/read/timeout)
instead of going silent.

(c) **The pending Orpheus `sample_rate` check, once the key exists.** The
build does NOT send `sample_rate` — it accepts whatever rate Groq returns
and resamples on-device, so the check is about efficiency, not correctness.
Watch the `reply audio:` line: if Groq returns 24 kHz (Orpheus's native
rate), nothing needs to change; only an unexpected rate (e.g. 8 kHz, thin
audio) would make a `sample_rate` parameter worth adding.

## Way back

Re-flash the first voice build's `muse-gadget.bin`
(`92ab854317bc5688...`, hash in VOICE_BUILD_REPORT.md; the binary itself is
on the laptop at `C:\Users\2keys\Downloads\firmware-aipi-voice\` from the
first flash) or the multi-Wi-Fi-only firmware from `firmware-aipi-multifi`,
or re-run muse.aipi.com/setup.html → official Muse 1.0.4 (NVS survives:
token, pairing, saved Wi-Fi, voice settings).
