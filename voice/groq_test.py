r"""Test the Groq TTS endpoint from the laptop, mirroring the box's request.

Reads the key from C:\Users\2keys\groq-key.txt (never prints it).
POSTs "Hello, Mike." to api.groq.com/openai/v1/audio/speech with the
same body the firmware sends, then prints the HTTP status, content
type/length, and WAV header details, and saves the audio to
C:\Users\2keys\groq-test.wav so it can be played.
"""
import json
import struct
import sys
import urllib.error
import urllib.request

KEY_PATH = r"C:\Users\2keys\groq-key.txt"
OUT_WAV = r"C:\Users\2keys\groq-test.wav"
URL = "https://api.groq.com/openai/v1/audio/speech"

with open(KEY_PATH, "r", encoding="utf-8") as f:
    raw = f.read().strip()
key = raw
if not key.startswith("gsk_"):
    for tok in raw.replace('"', " ").replace("'", " ").replace("=", " ").split():
        if tok.startswith("gsk_"):
            key = tok
            break
if not key.startswith("gsk_"):
    print("ERROR: no gsk_ key found in", KEY_PATH)
    sys.exit(1)

body = json.dumps({
    "model": "canopylabs/orpheus-v1-english",
    "input": "Hello, Mike.",
    "voice": "troy",
    "response_format": "wav",
}).encode()

req = urllib.request.Request(URL, data=body, headers={
    "Authorization": "Bearer " + key,
    "Content-Type": "application/json",
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
})
try:
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = resp.read()
        print("HTTP", resp.status)
        print("content-type:", resp.headers.get("Content-Type"))
        print("bytes:", len(data))
        if data[:4] == b"RIFF" and data[8:12] == b"WAVE":
            fmt = data.find(b"fmt ")
            if fmt >= 0:
                audio_fmt, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", data, fmt + 8)
                print("WAV: format=%d channels=%d rate=%d bits=%d" % (audio_fmt, channels, rate, bits))
            with open(OUT_WAV, "wb") as f:
                f.write(data)
            print("saved:", OUT_WAV)
        else:
            print("NOT A WAV. first 200 bytes:", data[:200])
except urllib.error.HTTPError as e:
    print("HTTP", e.code)
    print("error body:", e.read().decode("utf-8", "replace")[:500])
except Exception as e:
    print("ERROR:", type(e).__name__, e)
