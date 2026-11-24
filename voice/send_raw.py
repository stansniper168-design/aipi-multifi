r"""Send one raw line to the AIPI box over COM3 and print the reply.

Usage: python send_raw.py ">voice="
The bare >voice= readback prints URL/model/voice/key-set, never the key.
"""
import sys
import time

import serial

if len(sys.argv) < 2:
    print("usage: send_raw.py <line>")
    sys.exit(1)

line = sys.argv[1]
if not line.endswith("\n"):
    line += "\n"

s = serial.Serial("COM3", 115200, timeout=1)
time.sleep(0.5)
s.write(line.encode())
print("sent: %r" % line.strip())
t0 = time.time()
buf = b""
while time.time() - t0 < 8:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
sys.stdout.buffer.write(buf)
s.close()
