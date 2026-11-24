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

/*
 * Groq text-to-speech for spoken replies. The pure pieces (chunker, WAV
 * parser, request body, >voice= command parser) have no ESP-IDF in them and
 * are host-tested (tests/test_muse_tts_groq.py); the fetch task below is
 * ESP-IDF only and is left out of the host build by MUSE_TTS_GROQ_HOST_TEST.
 *
 * The fetch task POSTs each <=200-char chunk to the configured OpenAI-shaped
 * /audio/speech endpoint (Groq's Orpheus by default) and streams the WAV
 * body into the chat session, which plays it. Any failure -- a bad key, a
 * 429, a timeout, a non-WAV body -- ends the fetch, and the session falls
 * back to silent text for the rest of the message. Nothing here ever blocks
 * the chat task: the fetch runs on its own task, and the session only
 * consumes what the fetch hands it.
 */

#include "muse_tts_groq.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#ifndef MUSE_TTS_GROQ_HOST_TEST
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "muse_chat_priv.h"
#include "muse_settings.h"
#include "network_proxy.h"
#include "network_proxy_tls.h"
#endif

#ifndef MUSE_TTS_GROQ_HOST_TEST
static const char *TAG = "muse.tts_groq";
#endif

/* ---- Chunker ---- */

static size_t utf8_char_len(const uint8_t *p, size_t remaining)
{
    uint8_t c = p[0];
    size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    return n <= remaining ? n : remaining;
}

size_t muse_tts_groq_chunk(const char *text, size_t max_chars, size_t *offs, size_t *lens, size_t cap)
{
    if (!text || max_chars == 0) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)text;
    size_t len = strlen(text);
    size_t total = 0;
    size_t pos = 0;
    while (pos < len) {
        while (pos < len && isspace(p[pos])) {
            pos++;
        }
        if (pos >= len) {
            break;
        }
        size_t end = pos;
        size_t chars = 0;
        size_t last_sentence = 0; /* byte offset just past the last sentence end in the cap */
        size_t last_space = 0;    /* byte offset of the last space in the cap */
        while (end < len && chars < max_chars) {
            uint8_t c = p[end];
            if (c == '\n') {
                last_sentence = end + 1;
            } else if (c == '.' || c == '!' || c == '?') {
                size_t q = end + 1;
                while (q < len && (p[q] == '.' || p[q] == '!' || p[q] == '?')) {
                    q++;
                }
                if (q >= len || isspace(p[q])) {
                    last_sentence = q;
                }
            }
            if (isspace(c)) {
                last_space = end;
            }
            end += utf8_char_len(p + end, len - end);
            chars++;
        }
        size_t cut;
        if (end >= len) {
            cut = len;
        } else if (last_sentence > pos) {
            cut = last_sentence;
        } else if (last_space > pos) {
            cut = last_space;
        } else {
            cut = end; /* one long word: hard cut, still on a character boundary */
        }
        while (cut > pos && isspace(p[cut - 1])) {
            cut--;
        }
        if (cut == pos) {
            cut = end; /* degenerate (all punctuation): keep the cap's worth */
        }
        if (total < cap) {
            offs[total] = pos;
            lens[total] = cut - pos;
        }
        total++;
        pos = cut;
    }
    return total;
}

/* ---- WAV parser ---- */

void muse_tts_wav_init(muse_tts_wav_t *w)
{
    memset(w, 0, sizeof(*w));
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int muse_tts_wav_feed(muse_tts_wav_t *w, const uint8_t *data, size_t len, size_t *used)
{
    *used = 0;
    if (w->done) {
        return 0;
    }
    while (*used < len && !w->done) {
        if (w->skip) {
            size_t n = len - *used < w->skip ? len - *used : w->skip;
            w->skip -= n;
            *used += n;
            continue;
        }
        size_t need = w->stage == 0 ? 12 : w->stage == 1 ? 8 : 16;
        size_t avail = len - *used;
        size_t take = need - w->buf_len < avail ? need - w->buf_len : avail;
        memcpy(w->buf + w->buf_len, data + *used, take);
        w->buf_len += take;
        *used += take;
        if (w->buf_len < need) {
            return 0; /* need more input */
        }
        if (w->stage == 0) {
            if (memcmp(w->buf, "RIFF", 4) || memcmp(w->buf + 8, "WAVE", 4)) {
                return -1;
            }
            w->buf_len = 0;
            w->stage = 1;
        } else if (w->stage == 1) {
            uint32_t size = rd32(w->buf + 4);
            if (!memcmp(w->buf, "fmt ", 4)) {
                if (size < 16) {
                    return -1;
                }
                w->fmt_size = size > 0xffff ? 0xffff : (uint16_t)size;
                w->buf_len = 0;
                w->stage = 2;
            } else if (!memcmp(w->buf, "data", 4)) {
                if (!w->fmt_seen) {
                    return -1; /* data before fmt: not a WAV we can play */
                }
                w->done = true;
            } else {
                w->skip = size;
                w->buf_len = 0;
            }
        } else { /* stage 2: fmt body */
            if (rd16(w->buf) != 1) {
                return -1; /* PCM only */
            }
            w->channels = rd16(w->buf + 2);
            w->rate = rd32(w->buf + 4);
            w->bits = rd16(w->buf + 14);
            if (!w->channels || w->channels > 2 || !w->rate || (w->bits != 8 && w->bits != 16)) {
                return -1;
            }
            w->fmt_seen = true;
            if (w->fmt_size > 16) {
                w->skip = w->fmt_size - 16;
            }
            w->buf_len = 0;
            w->stage = 1;
        }
    }
    return 0;
}

/* ---- Request body ---- */

static size_t json_escaped_len(const char *s, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        n += c == '"' || c == '\\' ? 2 : c < 0x20 ? 6 : 1;
    }
    return n;
}

static char *json_escape_into(char *out, const char *s, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') {
            *out++ = '\\';
            *out++ = '"';
        } else if (c == '\\') {
            *out++ = '\\';
            *out++ = '\\';
        } else if (c < 0x20) {
            *out++ = '\\';
            *out++ = 'u';
            *out++ = '0';
            *out++ = '0';
            *out++ = hex[c >> 4];
            *out++ = hex[c & 0xf];
        } else {
            *out++ = (char)c;
        }
    }
    return out;
}

char *muse_tts_groq_body(const char *model, const char *voice, const char *text, size_t len)
{
    static const char head[] = "{\"model\":\"";
    static const char mid1[] = "\",\"voice\":\"";
    static const char mid2[] = "\",\"input\":\"";
    static const char tail[] = "\",\"response_format\":\"wav\"}";
    size_t n = sizeof(head) - 1 + json_escaped_len(model, strlen(model)) + sizeof(mid1) - 1 +
               json_escaped_len(voice, strlen(voice)) + sizeof(mid2) - 1 + json_escaped_len(text, len) +
               sizeof(tail) - 1;
    char *body = malloc(n + 1);
    if (!body) {
        return NULL;
    }
    char *p = body;
    memcpy(p, head, sizeof(head) - 1);
    p += sizeof(head) - 1;
    p = json_escape_into(p, model, strlen(model));
    memcpy(p, mid1, sizeof(mid1) - 1);
    p += sizeof(mid1) - 1;
    p = json_escape_into(p, voice, strlen(voice));
    memcpy(p, mid2, sizeof(mid2) - 1);
    p += sizeof(mid2) - 1;
    p = json_escape_into(p, text, len);
    memcpy(p, tail, sizeof(tail) - 1);
    p += sizeof(tail) - 1;
    *p = '\0';
    return body;
}

/* ---- >voice= command ---- */

int muse_tts_groq_parse_voice_cmd(const char *body, char *url, size_t url_cap, char *model, size_t model_cap,
                                  char *voice, size_t voice_cap, char *key, size_t key_cap)
{
    url[0] = model[0] = voice[0] = key[0] = '\0';
    if (!body || !body[0]) {
        return 1; /* bare ">voice=" is a readback */
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return -1;
    }
    cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "url");
    cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "model");
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "voice");
    cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "key");
    int rc = -1;
    if (!u && !m && !v && !k) {
        rc = 1; /* "{}" is a readback */
    } else if (cJSON_IsString(u) && u->valuestring && cJSON_IsString(m) && m->valuestring && cJSON_IsString(k) &&
               k->valuestring && (!v || cJSON_IsString(v)) && strncmp(u->valuestring, "https://", 8) == 0 &&
               u->valuestring[0] && m->valuestring[0] && k->valuestring[0] && strlen(u->valuestring) < url_cap &&
               strlen(m->valuestring) < model_cap && strlen(k->valuestring) < key_cap &&
               (!v || !v->valuestring || strlen(v->valuestring) < voice_cap)) {
        strlcpy(url, u->valuestring, url_cap);
        strlcpy(model, m->valuestring, model_cap);
        strlcpy(voice, v && v->valuestring && v->valuestring[0] ? v->valuestring : MUSE_TTS_GROQ_VOICE, voice_cap);
        strlcpy(key, k->valuestring, key_cap);
        rc = 0;
    }
    cJSON_Delete(root);
    return rc;
}

#ifndef MUSE_TTS_GROQ_HOST_TEST

/* ---- Fetch task ---- */

#define TTS_FETCH_STACK 8192
#define TTS_FETCH_PRIO 4         /* below the voice task: audio never starves for a download */
#define TTS_FETCH_DEADLINE_MS 60000 /* per chunk, from the first byte */
#define TTS_READ_LEN 2048

typedef struct {
    int msg;
    uint32_t gen;
    char *text; /* owned copy, up to TEXT_MAX - 1 bytes */
    char url[MUSE_TTS_URL_MAX + 1];
    char model[MUSE_TTS_MODEL_MAX + 1];
    char voice[MUSE_TTS_VOICE_MAX + 1];
    char key[MUSE_TTS_KEY_MAX + 1];
} tts_job_t;

typedef struct {
    int64_t deadline_us;
    bool timed_out;
} tts_fetch_ctx_t;

/* esp_http_client_read waits for the full buffer; a trickling server would
 * hold one call past the deadline. The event re-clamps it (main/image_fetch.c
 * does the same for downloads). */
static esp_err_t tts_http_event(esp_http_client_event_t *evt)
{
    tts_fetch_ctx_t *ctx = evt->user_data;
    if ((evt->event_id == HTTP_EVENT_ON_DATA || evt->event_id == HTTP_EVENT_ON_HEADER) &&
        esp_timer_get_time() > ctx->deadline_us) {
        ctx->timed_out = true;
    }
    return ESP_OK;
}

typedef enum { TTS_READ_OK, TTS_READ_STALE, TTS_READ_FAIL } tts_read_t;

/* POST one chunk and stream its WAV body into the session. `first` is the
 * message's first chunk: its WAV params become the stream's, and later chunks
 * must match them. */
static tts_read_t tts_fetch_chunk(tts_job_t *job, const char *text, size_t len, bool first, uint32_t *rate,
                                  uint16_t *channels, uint16_t *bits)
{
    char *body = muse_tts_groq_body(job->model, job->voice, text, len);
    if (!body) {
        ESP_LOGW(TAG, "Groq TTS request body build failed (OOM)");
        return TTS_READ_FAIL;
    }
    tts_read_t rc = TTS_READ_FAIL;
    esp_http_client_handle_t c = NULL;
    esp_transport_handle_t transport = NULL;
    uint8_t *buf = NULL;
    char *auth = NULL;
    network_proxy_config_t proxy;
    network_proxy_get_active(&proxy);
    bool use_proxy = proxy.type != NETWORK_PROXY_OFF;
    network_proxy_wipe(&proxy, sizeof(proxy));
    if (use_proxy) {
        transport = network_proxy_http_transport(true);
        if (!transport) {
            ESP_LOGW(TAG, "Groq TTS proxy transport creation failed");
            goto out;
        }
    }
    tts_fetch_ctx_t ctx = {.deadline_us = esp_timer_get_time() + (int64_t)TTS_FETCH_DEADLINE_MS * 1000,
                           .timed_out = false};
    esp_http_client_config_t cfg = {
        .url = job->url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .event_handler = tts_http_event,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
#if CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
    cfg.transport = transport;
#else
    if (transport) {
        ESP_LOGW(TAG, "Groq TTS custom transport unavailable in this build");
        esp_transport_destroy(transport);
        transport = NULL;
        goto out;
    }
#endif
    c = esp_http_client_init(&cfg);
    if (!c) {
        ESP_LOGW(TAG, "Groq TTS http client init failed");
        goto out;
    }
    auth = malloc(strlen(job->key) + 8);
    if (!auth) {
        ESP_LOGW(TAG, "Groq TTS auth header alloc failed (OOM)");
        goto out;
    }
    snprintf(auth, strlen(job->key) + 8, "Bearer %s", job->key);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    /* Cloudflare rejects the ESP32 client's default User-Agent ("ESP32 HTTP
     * Client/1.0") with HTTP 403 code 1010; a browser-like one gets through.
     * set_header replaces the default (see MUSE_TTS_GROQ_USER_AGENT). */
    esp_http_client_set_header(c, "User-Agent", MUSE_TTS_GROQ_USER_AGENT);
    /* Wipe the key the codebase's way (explicit_bzero is not in this newlib). */
    network_proxy_wipe(auth, strlen(auth));
    free(auth);
    auth = NULL;
    {
        esp_err_t err = esp_http_client_open(c, (int)strlen(body));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Groq TTS http open failed: %s (%d)", esp_err_to_name(err), err);
            goto out;
        }
    }
    {
        int written = esp_http_client_write(c, body, (int)strlen(body));
        if (written < 0) {
            ESP_LOGW(TAG, "Groq TTS http write failed: %d of %d bytes", written, (int)strlen(body));
            goto out;
        }
    }
    {
        esp_err_t err = esp_http_client_fetch_headers(c);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Groq TTS fetch headers failed: %s (%d)", esp_err_to_name(err), err);
            goto out;
        }
    }
    if (esp_http_client_get_status_code(c) != 200) {
        /* 401/429/5xx alike: no audio, fall back to silent text. */
        ESP_LOGW(TAG, "Groq TTS answered HTTP %d", esp_http_client_get_status_code(c));
        goto out;
    }
    buf = heap_caps_malloc(TTS_READ_LEN, MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGW(TAG, "Groq TTS read buffer alloc failed (OOM)");
        goto out;
    }
    {
        muse_tts_wav_t wav;
        muse_tts_wav_init(&wav);
        for (;;) {
            if (ctx.timed_out || !muse_chat_tts_alive(job->gen, job->msg)) {
                if (ctx.timed_out) {
                    ESP_LOGW(TAG, "Groq TTS chunk timed out after %d ms", TTS_FETCH_DEADLINE_MS);
                }
                rc = ctx.timed_out ? TTS_READ_FAIL : TTS_READ_STALE;
                goto out;
            }
            int n = esp_http_client_read(c, (char *)buf, TTS_READ_LEN);
            if (n < 0) {
                ESP_LOGW(TAG, "Groq TTS http read failed: %d", n);
                goto out;
            }
            if (n == 0) {
                rc = TTS_READ_OK; /* EOF: the whole chunk arrived */
                goto out;
            }
            size_t off = 0;
            if (!wav.done) {
                size_t used = 0;
                if (muse_tts_wav_feed(&wav, buf, (size_t)n, &used) != 0) {
                    ESP_LOGW(TAG, "Groq TTS reply is not a PCM WAV");
                    goto out;
                }
                off = used;
                if (wav.done) {
                    if (first) {
                        *rate = wav.rate;
                        *channels = wav.channels;
                        *bits = wav.bits;
                    } else if (wav.rate != *rate || wav.channels != *channels || wav.bits != *bits) {
                        ESP_LOGW(TAG, "Groq TTS chunk changed format mid-reply");
                        goto out;
                    }
                    muse_chat_tts_wav_header(job->gen, job->msg, wav.rate, wav.channels, wav.bits);
                }
            }
            while (off < (size_t)n) {
                int sent = muse_chat_tts_feed(job->gen, job->msg, buf + off, (size_t)n - off);
                if (sent < 0) {
                    rc = TTS_READ_STALE;
                    goto out;
                }
                if (sent == 0) {
                    /* The session's PCM buffer is full: wait for it to drain. */
                    vTaskDelay(pdMS_TO_TICKS(10));
                    if (!muse_chat_tts_alive(job->gen, job->msg)) {
                        rc = TTS_READ_STALE;
                        goto out;
                    }
                    continue;
                }
                off += (size_t)sent;
            }
        }
    }
out:
    free(buf);
    free(body);
    if (c) {
        esp_http_client_cleanup(c);
    }
    if (transport) {
        esp_transport_destroy(transport);
    }
    return rc;
}

static void tts_fetch_task(void *arg)
{
    tts_job_t *job = arg;
    size_t offs[MUSE_TTS_GROQ_MAX_CHUNKS];
    size_t lens[MUSE_TTS_GROQ_MAX_CHUNKS];
    size_t nchunks = muse_tts_groq_chunk(job->text, MUSE_TTS_GROQ_MAX_CHARS, offs, lens, MUSE_TTS_GROQ_MAX_CHUNKS);
    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0;
    tts_read_t rc = TTS_READ_FAIL;
    if (nchunks == 0 || nchunks > MUSE_TTS_GROQ_MAX_CHUNKS) {
        ESP_LOGW(TAG, "Groq TTS chunking failed: %u chunks", (unsigned)nchunks);
        goto done;
    }
    for (size_t i = 0; i < nchunks; i++) {
        rc = tts_fetch_chunk(job, job->text + offs[i], lens[i], i == 0, &rate, &channels, &bits);
        if (rc != TTS_READ_OK) {
            break;
        }
    }
done:
    if (rc == TTS_READ_OK) {
        muse_chat_tts_end(job->gen, job->msg, true);
    } else if (rc == TTS_READ_FAIL) {
        muse_chat_tts_end(job->gen, job->msg, false);
    }
    /* STALE: the turn moved on; the session already forgot this message. */
    free(job->text);
    free(job);
    vTaskDelete(NULL);
}

bool muse_tts_voice_configured(void)
{
    return muse_settings_tts_configured();
}

bool muse_tts_groq_start(int msg, const char *text, uint32_t gen)
{
    tts_job_t *job = malloc(sizeof(*job));
    if (!job) {
        ESP_LOGW(TAG, "Groq TTS job alloc failed (OOM)");
        return false;
    }
    memset(job, 0, sizeof(*job));
    job->msg = msg;
    job->gen = gen;
    job->text = strdup(text);
    if (!job->text) {
        ESP_LOGW(TAG, "Groq TTS text copy failed (OOM)");
        free(job);
        return false;
    }
    muse_settings_tts(job->url, job->model, job->voice, job->key);
    BaseType_t created = xTaskCreate(tts_fetch_task, "muse_tts", TTS_FETCH_STACK, job, TTS_FETCH_PRIO, NULL);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "Groq TTS fetch task failed to start: %d", (int)created);
        free(job->text);
        free(job);
        return false;
    }
    return true;
}

#endif /* MUSE_TTS_GROQ_HOST_TEST */
