/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/*
 * Groq text-to-speech for spoken replies (the AIPI Lite multi-Wi-Fi build's
 * voice step). The box has no TTS of its own: when a voice is configured,
 * the chat session starts a fetch task per message that POSTs the message's
 * text to an OpenAI-shaped /audio/speech endpoint (Groq by default) and
 * streams the returned WAV into the session task, which plays it like the
 * MP3 replies it used to fetch. With no voice configured, replies stay
 * silent text, byte-identical to before.
 *
 * This header is plain C: the chunker, WAV parser, request-body builder and
 * >voice= command parser are host-testable (tests/test_muse_tts_groq.py).
 * The fetch task itself is ESP-IDF only (muse_tts_groq.c, guarded by
 * MUSE_TTS_GROQ_HOST_TEST for the host build).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Defaults; the settings (muse_settings tts_*) override them at runtime. */
#define MUSE_TTS_GROQ_URL "https://api.groq.com/openai/v1/audio/speech"
#define MUSE_TTS_GROQ_MODEL "canopylabs/orpheus-v1-english"
#define MUSE_TTS_GROQ_VOICE "troy"
/* Orpheus caps a request at 200 characters; longer replies are chunked. */
#define MUSE_TTS_GROQ_MAX_CHARS 200
#define MUSE_TTS_GROQ_MAX_CHUNKS 8
/* Cloudflare (in front of Groq) rejects the ESP32 HTTP client's default
 * User-Agent ("ESP32 HTTP Client/1.0") with HTTP 403, code 1010, so the
 * fetch sends a browser-like one instead. esp_http_client_set_header
 * replaces the default (http_header_set overwrites the key). Keep this a
 * named constant so it is easy to change. */
#define MUSE_TTS_GROQ_USER_AGENT \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
/* One Groq request is at most ~1023 chars of text, so 8 chunks always hold it. */

/*
 * Split `text` (NUL-terminated UTF-8) into chunks of at most `max_chars`
 * characters, cutting at sentence boundaries (".", "!", "?", newline) when
 * one falls inside the cap, else at a word boundary, else hard at the cap
 * on a character boundary (a single word longer than the cap). Leading and
 * trailing whitespace is trimmed from each chunk; empty chunks are skipped.
 * Writes at most `cap` (offset, length) pairs, in bytes. Returns the total
 * number of chunks the text needs, which may exceed `cap`; the caller fails
 * soft (silent text) if it does.
 */
size_t muse_tts_groq_chunk(const char *text, size_t max_chars, size_t *offs, size_t *lens, size_t cap);

/* Incremental WAV header parser. A Groq reply is a whole WAV file per chunk,
 * so each chunk's response is parsed separately and its header stripped. */
typedef struct {
    uint32_t rate;       /* sample rate (Hz), once the header is complete */
    uint16_t channels;   /* 1 or 2 */
    uint16_t bits;       /* 8 or 16 */
    bool done;           /* header complete: bytes after it are PCM */
    bool fmt_seen;       /* a fmt chunk was parsed (a data chunk before it is invalid) */
    uint8_t buf[64];     /* unparsed header bytes */
    size_t buf_len;
    size_t skip;         /* bytes of the current unknown subchunk still to discard */
    int stage;           /* 0 = RIFF header, 1 = subchunk header, 2 = fmt body */
    uint16_t fmt_size;
} muse_tts_wav_t;

void muse_tts_wav_init(muse_tts_wav_t *w);
/*
 * Feed bytes. Returns 0 on success, -1 if the header is not a PCM WAV.
 * `*used` is how many bytes of `data` belong to the header (0..len); once
 * `w->done` is true, the bytes after `*used` are PCM.
 */
int muse_tts_wav_feed(muse_tts_wav_t *w, const uint8_t *data, size_t len, size_t *used);

/*
 * Build the JSON body for one chunk: {"model","voice","input","response_format":"wav"}.
 * The text is JSON-escaped. Returns a heap string the caller frees, or NULL on OOM.
 */
char *muse_tts_groq_body(const char *model, const char *voice, const char *text, size_t len);

/*
 * Parse a `>voice=` console command body. Returns 1 for a readback (empty
 * body or an object with no voice fields), 0 for a set (the four fields are
 * filled; voice defaults to MUSE_TTS_GROQ_VOICE when omitted), or -1 for an
 * invalid body (bad JSON, a non-https URL, or a missing/empty url, model or
 * key, or a value too long for its field). The key is never printed anywhere.
 */
int muse_tts_groq_parse_voice_cmd(const char *body, char *url, size_t url_cap, char *model, size_t model_cap,
                                  char *voice, size_t voice_cap, char *key, size_t key_cap);

/* True when a voice is configured in settings (https URL, model and key set). */
bool muse_tts_voice_configured(void);
/*
 * Start fetching speech for message `msg` (its text, up to TEXT_MAX - 1
 * bytes) on a dedicated task. `gen` is the turn generation, used to abandon
 * the fetch if the turn moves on. Returns false if the task could not start
 * (the session falls back to silent text).
 */
bool muse_tts_groq_start(int msg, const char *text, uint32_t gen);

#ifdef __cplusplus
}
#endif
