# AIPI Lite Groq voice build — build report

Built 2026-11-24 ~03:26 UTC (Nov 23 ~21:26 CST) from facebookincubator/muse-gadget-sdk,
HEAD `e17081a`, ESP-IDF v6.0.3, `aipi` profile. This is the multi-Wi-Fi build
**plus the Groq voice patch** (VOICE_SCOPING_REPORT.md): spoken replies on the
box via Groq's Orpheus TTS, no OpenAI anywhere.

## Status: BUILD SUCCEEDED — firmware ready, NOT flashed

`BUILD_EXIT=0`. Fresh build from scratch (the copied `build-muse-aipi/` was
deleted first — its CMakeCache pointed at the old `aipi-multifi-scope` tree).
Build took ~9.5 minutes with `CMAKE_BUILD_PARALLEL_LEVEL=2`. One expected
warning: **"No SDK token"** — the build is token-less, exactly like the
multi-Wi-Fi build; the token is read from the device's NVS at runtime, and
Mike's is already stored there. No token value was asked for, used, or seen.

- App version: **999.0.0** (dev build, same as the multi-Wi-Fi build).
- `muse-gadget.bin` is 0x221000 bytes (2,232,320); **47% free** in the 4 MB ota_0 slot.
- `CONFIG_MUSE_TTS_GROQ=y` confirmed in the generated sdkconfig.

## What changed, per file

Work tree: `~/workspace/aipi-voice-build/` (a `cp -a` of the multi-Wi-Fi
source). **The known-good multi-Wi-Fi tree at `~/workspace/aipi-multifi-scope/`
was never edited.**

### New files

| File | Lines | What it is |
|---|---|---|
| `esp32/components/muse/muse_tts_groq.h` | ~120 | Public API: defaults, chunker, WAV parser, body builder, `>voice=` parser, fetch-task entry points. Plain C with `extern "C"` guards; host-testable. |
| `esp32/components/muse/muse_tts_groq.c` | ~560 | The patch. Pure pieces (chunker, WAV parser, body, command parser) + the ESP-IDF fetch task. |
| `esp32/tests/test_muse_tts_groq.py` | ~60 | Host test runner for the pure pieces. |
| `esp32/tests/test_muse_tts_groq_harness.c` | ~300 | Host harness: chunker, WAV, body, command-parser cases. |

### Modified files

| File | Change |
|---|---|
| `esp32/components/muse/muse_chat_session.cpp` | +232/−17. `start_tts()` launches a Groq fetch when a voice is configured and the speaker is on, else silent text as before; fail fallback to silence; new `decode_wav()` (PCM → mono → resample → speaker); feed/end/header/alive API for the fetch task; 64 KB PCM stream buffer created in `muse_hatch_start()` (optional — voice stays off if it can't be made). MP3 path untouched. |
| `esp32/components/muse/muse_chat_priv.h` | Feed API declarations (`muse_chat_tts_feed`, `muse_chat_tts_wav_header`, `muse_chat_tts_end`, `muse_chat_tts_alive`). |
| `esp32/components/muse/muse_settings.{c,h}` | Voice settings in the `"muse"` NVS namespace: keys `tts_url`, `tts_model`, `tts_voice`, `tts_key`; `muse_settings_tts_configured()` (https URL + model + key, else voice off) and `muse_settings_tts()` / `muse_settings_set_tts()`. Load-time validation clears a half-saved or non-https voice. |
| `esp32/components/muse/Kconfig` | `CONFIG_MUSE_TTS_GROQ` (bool, depends on `MUSE_HATCH && SPIRAM`, default y). |
| `esp32/components/muse/CMakeLists.txt` | `muse_tts_groq.c` added when `CONFIG_MUSE_TTS_GROQ` is on. |
| `esp32/main/sdk_token_console.c` | `>voice={...}` JSON set + `>voice=` readback, mirroring `>wifi_proxy=`. Readback prints URL/model/voice/key-set only — **never the key**. The line buffer is wiped after the command runs (existing `feed()` behavior). |
| `esp32/tests/test_muse_chat_session.py` | +127. `-DCONFIG_MUSE_TTS_GROQ=1`, TTS stubs in the harness preamble, 7 new cases (43–49). |

## How it works

1. **Launch** (`start_tts`): for each spoken reply, when a voice is configured
   (`muse_settings_tts_configured()`), the speaker is on, volume > 0, the
   message text was kept, and the PCM buffer exists, the session starts a
   fetch task with the message's kept text (up to 1023 chars) and a fresh
   generation number. Anything else — no voice, speaker off/muted, no text,
   task won't start — stays silent text, paced over the reading-pace estimate
   exactly as before. Typed turns never start a fetch.
2. **Fetch** (`tts_fetch_task`, priority 4, 8 KB stack — below the voice task,
   so audio never starves for a download): chunks the text at sentence
   boundaries (≤200 chars/request, word-boundary fallback, hard cut on a
   character boundary for one long word, UTF-8 safe), then POSTs each chunk to
   the configured endpoint with `Authorization: Bearer <key>` (cert bundle,
   proxy transport, same pattern as `muse_account_api.c`), 60 s deadline per
   chunk enforced by the HTTP event handler (copied from `main/image_fetch.c`).
   Each chunk's reply is a whole WAV file: the header is parsed and stripped
   (PCM only, 8/16-bit, mono/stereo), later chunks must match the first
   chunk's format, and the PCM is fed to the session with backpressure
   (waits 10 ms when the session's PCM buffer is full).
3. **Play** (`decode_wav`): the session task pulls PCM from the 64 KB stream
   buffer, converts to mono 16-bit, resamples to the speaker's 16 kHz with the
   existing resampler, and pushes to `s_out`. Batches are capped by both the
   resampled-output buffer and the speaker room left, so a partial speaker
   send can't clip audio.
4. **Fail soft**: any fetch failure (bad key, HTTP 429/5xx, timeout, non-WAV
   body, format change mid-reply) ends the fetch; the session falls back to
   silent text for the rest of the message (paced over the text estimate) when
   nothing was spoken yet, or finishes the message with the audio already
   played. A fetch that outlives its turn is ignored by generation + message
   checks, so it can never feed the next message's audio. Rate limits cost
   nothing but the silence: the fetch stops at the first failed chunk.

## Deviations from the scoping report (with evidence)

1. **Settings live in the `"muse"` NVS namespace via `muse_settings`, not
   `"homehub"`** (report §1). `config_store.c`/`"homehub"` belongs to Link's
   daemon side; the Muse component's own settings (SSID, token, host) all live
   in `"muse"` under `muse_settings.c`. The keys are exactly the report's
   `tts_url`, `tts_model`, `tts_voice`, `tts_key`.
2. **The stub comment's recipe is superseded, not followed literally.** The
   `start_tts()` stub assumed an on-task MP3 fetch via `tts_data()`; the build
   uses a dedicated fetch task + WAV + stream buffer instead, because a
   blocking HTTPS POST on the session task would stall dictation/chat, and
   Orpheus is WAV-only. `pcm_frames` is set to the text estimate (not 0) so
   the silent fallback has a sane duration.
3. **Cross-task state uses atomics + a stream buffer, not a mutex.** The
   session's `turn_t` can't hold atomics (`s_turn = turn_t{};` needs a
   copy-assignable struct), so the fetch/session shared state lives in
   file-scope atomics (`s_tts_failed`, `s_tts_gen`, `s_tts_ended`,
   `s_tts_wav_header_seen/rate/channels/bits`) and the PCM travels in a
   dedicated 64 KB stream buffer (`s_tts_pcm`). Same check-then-send idiom
   the MP3 path already uses.
4. **`muse_settings_tts()` defaults an empty voice to `"troy"`.** The console
   parser also defaults a missing voice to `"troy"`. The endpoint/model/key
   are required; the voice is not.
5. **No console command to clear the voice yet.** Muting the speaker silences
   replies without spending Groq requests; reflashing the multi-Wi-Fi-only
   firmware removes the voice code (the stored keys then sit unused in NVS).

## Host test results

| Suite | Result |
|---|---|
| `tests/test_muse_tts_groq.py` (new) | **PASS** — chunker (sentence/word/hard cuts, UTF-8, ellipsis), WAV parser (canonical header byte-at-a-time and whole, extra LIST chunk, fmt size 18, data-before-fmt, non-PCM, 8-bit mono), request body (exact JSON + escaping), `>voice=` parser (readbacks, full set, voice default, http URL, missing key/model, bad JSON, over-long value). Compiled with ASan+UBSan, `-Wall -Wextra -Werror`. |
| `tests/test_muse_chat_session.py` (full) | **48 tests PASS** — the 41 pre-existing tests plus 7 new TTS cases (43–49): fetch starts when configured; silent without voice, with speaker off, when muted, when the task won't start; failed fetch falls back to silence; typed replies never start a fetch. |

## Artifacts (in `firmware-aipi-voice/`)

| File | Address | Size | SHA-256 |
|---|---|---|---|
| bootloader.bin | 0x0 | 22,752 | `4d65f4fb2782be4fb34f917e3dd2e97e500b9defdfa83b45a754d10db4c5cdf1` |
| partition-table.bin | 0x10000 | 3,072 | `f1eed33f4d8c528ce6085b46abfeb3a176beefe71601330a81c1cc15fc58e08f` |
| ota_data_initial.bin | 0x1d000 | 8,192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| muse-gadget.bin | 0x20000 | 2,232,320 | `92ab854317bc5688fac4fae4496e8884f2a20aae8bfb94b23d9ec49a3c58e746` |

Zip: `firmware-aipi-voice.zip` (1,488,276 bytes) — the folder plus README.md and
flash.bat. The partition table and ota_data_initial hashes match the
multi-Wi-Fi build; the bootloader hash differs (rebuilt against the new
sdkconfig — expected); muse-gadget.bin differs (the voice patch).

## Verification — voice is in the binary

- `muse_tts_groq_start`, `muse_chat_tts_feed`, `muse_chat_tts_end`,
  `decode_wav` all present in `muse-gadget.elf` (nm).
- Log strings present: `"speaking message %s (%u chars)"`,
  `"Groq TTS answered HTTP %d"`, `"Groq TTS reply is not a PCM WAV"`,
  `"Groq TTS chunk changed format mid-reply"`.
- `CONFIG_MUSE_TTS_GROQ=y` in `build-muse-aipi/sdkconfig`.

## Flash procedure (Windows laptop, esptool.py — same as the multi-Wi-Fi flash)

**Do NOT flash without Mike present. Flash/test is a later step with him.**

```bat
python -m esptool --chip esp32s3 -p COM3 -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size keep --flash-freq 80m 0x0 bootloader.bin 0x10000 partition-table.bin 0x1d000 ota_data_initial.bin 0x20000 muse-gadget.bin
```

Or `flash.bat COM3` from the `firmware-aipi-voice/` folder (already on the
laptop from the multi-Wi-Fi flash — copy the new folder over). **Never
`erase-flash`**: it wipes pairing, the SDK token, saved Wi-Fi, and the voice
settings. A normal flash preserves NVS, so the token, pairing, both saved
Wi-Fi networks, and any voice settings survive.

## What is still needed from Mike before flash/test

(a) **Groq API key — DONE on his side, not needed in chat.** Per the memory
update Nov 23 evening, Mike already created a free Groq account + API key and
saved it in a Windows Notepad file on the laptop. **He enters it himself at
flash time, never in chat.** The command to send over the box's serial console
(115200 baud, after flashing, box on the USB-C data cable):

```
>voice={"url":"https://api.groq.com/openai/v1/audio/speech","model":"canopylabs/orpheus-v1-english","voice":"troy","key":"gsk_..."}
```

The box answers `@voice saved`. A bare `>voice=` prints the status (URL,
model, voice, key-set) and **never prints the key**.

(b) **The box plugged into the laptop via the USB-C data cable, laptop awake**
— same as the multi-Wi-Fi flash (COM3 last time).

(c) **The pending Orpheus `sample_rate` check, once the key exists.** The
scoping report flagged it as open: whether Orpheus honors `sample_rate=16000`
(the build does NOT send it — it accepts whatever rate Groq returns and
resamples on-device, so the check is about efficiency, not correctness). At
flash/test time, watch the first spoken reply's boot log for the
`"reply audio: %u Hz, %u ch, WAV"` line: if Groq returns 24 kHz (Orpheus's
native rate), the resampler handles it and nothing needs to change; if it
returns 48 kHz, same. Only if the rate is unexpected (e.g. 8 kHz, thin audio)
is a `sample_rate` parameter worth adding to the request body.

## Known limitations / risks

- **Orpheus WAV-only, 200 chars/request, free tier ~100 req/day.** A 1023-char
  reply is up to 6 requests; a long conversation can burn the daily quota, and
  the 101st request fails soft to silent text. No retry, no queue.
- **No per-message voice selection and no voice-off command yet** (see
  deviations §5). The voice is one global setting.
- **Groq model churn is the highest non-technical risk** (Groq already killed
  one TTS model, playai-tts). If `canopylabs/orpheus-v1-english` is retired,
  the endpoint returns an error and replies go silent; the fix is a new model
  string via `>voice=`, no reflash.
- **Untested on hardware.** The host tests cover the pure logic and the
  session launch/fallback; the fetch task, TLS, and real audio have only been
  compiled, not run. First flash/test with Mike must confirm: the box boots
  and rejoins Wi-Fi (multi-Wi-Fi intact), `>voice=` saves and reports, a
  spoken reply actually speaks, and a failed fetch (e.g. wrong key) falls
  back to silent text without breaking the turn.

## Way back

Re-run muse.aipi.com/setup.html → official Muse 1.0.4 (NVS survives: token,
pairing, saved Wi-Fi, voice settings). Or re-flash `firmware-aipi-multifi`
(multi-Wi-Fi only, voice code gone). Or restore the stock backup
(`aipi-backup-D4154C.bin`) via the same installer flow.
