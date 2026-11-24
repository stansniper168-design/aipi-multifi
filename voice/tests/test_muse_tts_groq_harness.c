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

/* Host harness for the Groq TTS pure pieces (muse_tts_groq.c, compiled with
 * MUSE_TTS_GROQ_HOST_TEST so the ESP-IDF fetch task is left out): the
 * chunker, the WAV parser, the request-body builder and the >voice= command
 * parser. Prints "tts groq harness: PASS" on success; any failed assert
 * aborts with the file and line. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "muse_tts_groq.h"

static void check_chunk(const char *text, size_t max_chars, size_t expect_count)
{
    size_t offs[MUSE_TTS_GROQ_MAX_CHUNKS];
    size_t lens[MUSE_TTS_GROQ_MAX_CHUNKS];
    size_t n = muse_tts_groq_chunk(text, max_chars, offs, lens, MUSE_TTS_GROQ_MAX_CHUNKS);
    assert(n == expect_count);
    size_t pos = 0;
    for (size_t i = 0; i < n && i < MUSE_TTS_GROQ_MAX_CHUNKS; i++) {
        /* Chunks are in order, non-empty, within the text and the cap. */
        assert(offs[i] >= pos && offs[i] + lens[i] <= strlen(text) && lens[i] > 0);
        size_t chars = 0;
        for (size_t j = 0; j < lens[i]; j++) {
            if ((text[offs[i] + j] & 0xc0) != 0x80) {
                chars++;
            }
        }
        assert(chars <= max_chars);
        /* No chunk starts or ends with whitespace. */
        assert(text[offs[i]] != ' ' && text[offs[i]] != '\n');
        assert(text[offs[i] + lens[i] - 1] != ' ' && text[offs[i] + lens[i] - 1] != '\n');
        pos = offs[i] + lens[i];
    }
}

static void test_chunker(void)
{
    assert(muse_tts_groq_chunk("", 200, NULL, NULL, 0) == 0);
    assert(muse_tts_groq_chunk("   \n  ", 200, NULL, NULL, 0) == 0);
    check_chunk("Hello there.", 200, 1);
    check_chunk("Hello there. How are you today? I am fine!", 200, 1);
    /* 210 chars, one sentence: the word boundary is the only cut. */
    char long_one[220];
    memset(long_one, 'a', 100);
    long_one[100] = ' ';
    memset(long_one + 101, 'b', 109);
    long_one[210] = '\0';
    size_t offs[8], lens[8];
    size_t n = muse_tts_groq_chunk(long_one, 200, offs, lens, 8);
    assert(n == 2);
    assert(offs[0] == 0 && lens[0] == 100); /* cut at the space */
    assert(offs[1] == 101 && lens[1] == 109);
    /* Sentences: cut at the sentence end inside the cap, not at the cap. */
    char sentences[400];
    size_t at = 0;
    for (int i = 0; i < 5; i++) {
        at += (size_t)snprintf(sentences + at, sizeof(sentences) - at, "Sentence number %d ends here. ", i + 1);
    }
    sentences[at] = '\0';
    n = muse_tts_groq_chunk(sentences, 200, offs, lens, 8);
    assert(n == 1); /* 5 x ~29 chars fits in 200 */
    /* Longer: each chunk ends at a sentence end. */
    char many[1200];
    at = 0;
    for (int i = 0; i < 30; i++) {
        at += (size_t)snprintf(many + at, sizeof(many) - at, "Sentence number %d ends here. ", i + 1);
    }
    many[at] = '\0';
    n = muse_tts_groq_chunk(many, 200, offs, lens, 8);
    assert(n > 1 && n <= 8);
    for (size_t i = 0; i < n; i++) {
        /* Every chunk but the last ends with a period. */
        if (i + 1 < n) {
            assert(many[offs[i] + lens[i] - 1] == '.');
        }
    }
    /* One word longer than the cap: hard cut, still whole characters. */
    char word[250];
    memset(word, 'x', sizeof(word) - 1);
    word[sizeof(word) - 1] = '\0';
    n = muse_tts_groq_chunk(word, 200, offs, lens, 8);
    assert(n == 2 && lens[0] == 200 && lens[1] == 49);
    /* Multi-byte characters are never split. */
    char utf8[300];
    at = 0;
    for (int i = 0; i < 60; i++) {
        memcpy(utf8 + at, "\xc3\xa9", 2); /* e-acute, 2 bytes, 1 char */
        at += 2;
    }
    utf8[at] = '\0';
    n = muse_tts_groq_chunk(utf8, 50, offs, lens, 8);
    assert(n == 2 && lens[0] == 100 && lens[1] == 20); /* 50 chars = 100 bytes */
    /* An ellipsis stays with its sentence, and the cut takes the last
     * sentence end inside the cap. */
    check_chunk("Wait... really? Yes!", 200, 1);
    n = muse_tts_groq_chunk("Wait... really? Yes! And then some more text follows here.", 20, offs, lens, 8);
    assert(n == 3);
    assert(offs[0] == 0 && lens[0] == 20);   /* "Wait... really? Yes!" */
    assert(offs[1] == 21 && lens[1] == 18);  /* "And then some more" */
    assert(offs[2] == 40 && lens[2] == 18);  /* "text follows here." */
}

/* A canonical 44-byte WAV header: 16-bit mono, 24000 Hz, 4 bytes of PCM. */
static const uint8_t wav44[] = {
    'R', 'I', 'F', 'F', 40, 0, 0, 0, 'W', 'A', 'V', 'E',
    'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0xc0, 0x5d, 0, 0, 0x80, 0xbb, 0, 0, 2, 0, 16, 0,
    'd', 'a', 't', 'a', 4, 0, 0, 0, 0x11, 0x22, 0x33, 0x44,
};

static void test_wav(void)
{
    muse_tts_wav_t w;
    size_t used = 0;
    /* All at once: the header is consumed, the PCM is left over. */
    muse_tts_wav_init(&w);
    assert(muse_tts_wav_feed(&w, wav44, sizeof(wav44), &used) == 0);
    assert(w.done && used == 44 && w.rate == 24000 && w.channels == 1 && w.bits == 16);
    /* One byte at a time: same result. */
    muse_tts_wav_init(&w);
    size_t total = 0;
    for (size_t i = 0; i < sizeof(wav44); i++) {
        used = 0;
        assert(muse_tts_wav_feed(&w, wav44 + i, 1, &used) == 0);
        total += used;
        if (w.done) {
            break;
        }
    }
    assert(w.done && total == 44 && w.rate == 24000);
    /* Feeding after done consumes nothing. */
    used = 99;
    assert(muse_tts_wav_feed(&w, wav44, 4, &used) == 0 && used == 0);
    /* Not a WAV. */
    static const uint8_t junk[] = {'N', 'O', 'P', 'E', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    muse_tts_wav_init(&w);
    assert(muse_tts_wav_feed(&w, junk, sizeof(junk), &used) == -1);
    /* Data before fmt. */
    uint8_t bad[44];
    memcpy(bad, wav44, sizeof(bad));
    memcpy(bad + 12, "data", 4);
    memcpy(bad + 36, "fmt ", 4);
    muse_tts_wav_init(&w);
    assert(muse_tts_wav_feed(&w, bad, sizeof(bad), &used) == -1);
    /* Non-PCM format (3 = float). */
    memcpy(bad, wav44, sizeof(bad));
    bad[20] = 3;
    muse_tts_wav_init(&w);
    assert(muse_tts_wav_feed(&w, bad, sizeof(bad), &used) == -1);
    /* An extra LIST chunk before data, and a fmt chunk with 2 extra bytes. */
    uint8_t extra[] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 18, 0, 0, 0, 1, 0, 2, 0, 0x44, 0xac, 0, 0, 0x10, 0xb1, 0x02, 0, 4, 0, 16, 0,
        0, 0, /* fmt extension */
        'L', 'I', 'S', 'T', 4, 0, 0, 0, 'a', 'b', 'c', 'd',
        'd', 'a', 't', 'a', 2, 0, 0, 0, 0x55, 0x66,
    };
    muse_tts_wav_init(&w);
    assert(muse_tts_wav_feed(&w, extra, sizeof(extra), &used) == 0);
    assert(w.done && w.rate == 44100 && w.channels == 2 && w.bits == 16);
    assert(used == sizeof(extra) - 2); /* the 2 PCM bytes are left over */
    /* 8-bit mono: bits 8 at fmt offset 34. */
    uint8_t eight[44];
    memcpy(eight, wav44, sizeof(eight));
    eight[34] = 8;
    eight[35] = 0;
    muse_tts_wav_init(&w);
    assert(muse_tts_wav_feed(&w, eight, sizeof(eight), &used) == 0);
    assert(w.done && w.bits == 8 && w.channels == 1);
}

static void test_body(void)
{
    char *b = muse_tts_groq_body("canopylabs/orpheus-v1-english", "troy", "Hello", 5);
    assert(b && !strcmp(b, "{\"model\":\"canopylabs/orpheus-v1-english\",\"voice\":\"troy\",\"input\":\"Hello\","
                          "\"response_format\":\"wav\"}"));
    free(b);
    /* Quotes, backslashes and control characters are escaped. */
    b = muse_tts_groq_body("m", "v", "a\"b\\c\nd", 7);
    assert(b && !strcmp(b, "{\"model\":\"m\",\"voice\":\"v\",\"input\":\"a\\\"b\\\\c\\u000ad\","
                          "\"response_format\":\"wav\"}"));
    free(b);
}

static void test_voice_cmd(void)
{
    char url[129], model[65], voice[33], key[129];
    /* Readbacks. */
    assert(muse_tts_groq_parse_voice_cmd("", url, sizeof(url), model, sizeof(model), voice, sizeof(voice), key,
                                        sizeof(key)) == 1);
    assert(muse_tts_groq_parse_voice_cmd("{}", url, sizeof(url), model, sizeof(model), voice, sizeof(voice), key,
                                        sizeof(key)) == 1);
    /* A full set, with the voice defaulted. */
    assert(muse_tts_groq_parse_voice_cmd(
               "{\"url\":\"https://api.groq.com/openai/v1/audio/speech\",\"model\":\"canopylabs/orpheus-v1-english\","
               "\"key\":\"gsk_abc\"}",
               url, sizeof(url), model, sizeof(model), voice, sizeof(voice), key, sizeof(key)) == 0);
    assert(!strcmp(url, "https://api.groq.com/openai/v1/audio/speech"));
    assert(!strcmp(model, "canopylabs/orpheus-v1-english"));
    assert(!strcmp(voice, "troy"));
    assert(!strcmp(key, "gsk_abc"));
    /* An explicit voice is kept. */
    assert(muse_tts_groq_parse_voice_cmd(
               "{\"url\":\"https://example.test/speech\",\"model\":\"m1\",\"voice\":\"hannah\",\"key\":\"k\"}",
               url, sizeof(url), model, sizeof(model), voice, sizeof(voice), key, sizeof(key)) == 0);
    assert(!strcmp(voice, "hannah"));
    /* Invalid: bad JSON, http URL, missing key, missing model, empty key. */
    assert(muse_tts_groq_parse_voice_cmd("{nope", url, sizeof(url), model, sizeof(model), voice, sizeof(voice), key,
                                        sizeof(key)) == -1);
    assert(muse_tts_groq_parse_voice_cmd(
               "{\"url\":\"http://example.test/speech\",\"model\":\"m1\",\"key\":\"k\"}",
               url, sizeof(url), model, sizeof(model), voice, sizeof(voice), key, sizeof(key)) == -1);
    assert(muse_tts_groq_parse_voice_cmd(
               "{\"url\":\"https://example.test/speech\",\"model\":\"m1\"}",
               url, sizeof(url), model, sizeof(model), voice, sizeof(voice), key, sizeof(key)) == -1);
    assert(muse_tts_groq_parse_voice_cmd(
               "{\"url\":\"https://example.test/speech\",\"key\":\"k\"}",
               url, sizeof(url), model, sizeof(model), voice, sizeof(voice), key, sizeof(key)) == -1);
    assert(muse_tts_groq_parse_voice_cmd(
               "{\"url\":\"https://example.test/speech\",\"model\":\"m1\",\"key\":\"\"}",
               url, sizeof(url), model, sizeof(model), voice, sizeof(voice), key, sizeof(key)) == -1);
    /* A value too long for its field is refused, not truncated. */
    char big[200];
    memset(big, 'k', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    char cmd[400];
    snprintf(cmd, sizeof(cmd),
             "{\"url\":\"https://example.test/speech\",\"model\":\"m1\",\"key\":\"%s\"}", big);
    assert(muse_tts_groq_parse_voice_cmd(cmd, url, sizeof(url), model, sizeof(model), voice, sizeof(voice), key,
                                        sizeof(key)) == -1);
}

static void test_user_agent(void)
{
    /* The fetch sends a browser-like User-Agent: Cloudflare (in front of
     * Groq) rejects the ESP32 client's default with HTTP 403, code 1010.
     * The header itself is set in the ESP-IDF-only fetch task, so the host
     * test checks the exact string it will send. */
    const char *ua = MUSE_TTS_GROQ_USER_AGENT;
    assert(strncmp(ua, "Mozilla/5.0", 11) == 0);
    assert(strstr(ua, "Chrome/") != NULL);
    assert(strstr(ua, "ESP32") == NULL);
}

int main(void)
{
    test_chunker();
    test_wav();
    test_body();
    test_voice_cmd();
    test_user_agent();
    printf("tts groq harness: PASS\n");
    return 0;
}
