# SPDX-License-Identifier: MIT
# Copyright (c) 2026 SigPro Labs, LLC
"""Upload a raw PCM file to the RadioSonic flash sample partition over the
TinyUSB console, following docs/SERIAL_XFER.md ("xfer put flash").

    python scripts/rs_put_flash.py COM7 sample.raw

The file must be 48 kHz, stereo, 32-bit signed little-endian PCM with no
header; that is the only format flash playback accepts. ffmpeg makes one from
any WAV, here the first 524288 samples (10.92 s) so a loop of the sample has
a whole number of 16384-sample periods:

    ffmpeg -i in.wav -af "atrim=end_sample=524288" -ac 2 -ar 48000 \\
           -f s32le -acodec pcm_s32le sample.raw

Afterwards `play start flash --loop` plays it into the DSP input in place of
the live audio. Needs pyserial (the IDF Python environment has it). A 4 MB
file takes about 25 s.
"""
import base64
import sys
import time
import zlib

import serial

CHUNK = 3072            # raw bytes per D: line (the device's limit)


def readline(port):
    return port.readline().decode(errors="replace").rstrip("\r\n")


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    com, path = argv[1], argv[2]
    payload = open(path, "rb").read()
    if len(payload) % 8:
        print(f"{path}: {len(payload)} bytes is not a whole number of stereo 32-bit frames")
        return 2
    crc = zlib.crc32(payload) & 0xFFFFFFFF

    port = serial.Serial(com, 115200, timeout=5, write_timeout=30)
    time.sleep(1.2)                      # the console's connect probe eats early bytes
    port.write(b"\n")
    time.sleep(0.3)
    port.reset_input_buffer()

    port.write(f"xfer put flash {len(payload)} {crc:08x}\n".encode())
    t0 = time.time()
    while True:
        line = readline(port)
        if line == "XFER READY":
            break
        if line.startswith("XFER ERR") or time.time() - t0 > 20:
            print("device did not accept the upload:", line or "(no reply)")
            return 1

    for i in range(0, len(payload), CHUNK):
        port.write(b"D:" + base64.b64encode(payload[i:i + CHUNK]) + b"\n")
        if (i // CHUNK) % 300 == 0:
            print(f"  {i / 1024:.0f} KB", flush=True)
    port.write(b"XFER DONE\n")

    t1 = time.time()
    while True:
        line = readline(port)
        if line.startswith("XFER"):
            rate = len(payload) / 1024 / (time.time() - t0)
            print(f"{line}  ({len(payload) / 1024:.0f} KB at {rate:.0f} KB/s)")
            return 0 if line == "XFER OK" else 1
        if time.time() - t1 > 60:
            print("no reply after XFER DONE")
            return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
