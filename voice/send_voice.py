r"""Send the >voice= command to the AIPI box over COM3.

Reads the Groq API key from C:\Users\2keys\groq-key.txt (a file Mike
saves himself from his Notepad — the key never goes through chat),
builds the >voice= JSON, sends it at 115200 baud, and prints the box's
reply. Expected reply: @voice saved
A bare >voice= readback never prints the key.
"""
import sys
import time

import serial

KEY_PATH = r"C:\Users\2keys\groq-key.txt"
PORT = "COM3"
BAUD = 115200

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

cmd = (
    '>voice={"url":"https://api.groq.com/openai/v1/audio/speech",'
    '"model":"canopylabs/orpheus-v1-english","voice":"troy",'
    '"key":"%s"}\n' % key
)

s = serial.Serial(PORT, BAUD, timeout=1)
time.sleep(0.5)
s.write(cmd.encode())
print("sent >voice= command (%d bytes), waiting for reply..." % len(cmd))
t0 = time.time()
buf = b""
while time.time() - t0 < 8:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
sys.stdout.buffer.write(buf)
s.close()
