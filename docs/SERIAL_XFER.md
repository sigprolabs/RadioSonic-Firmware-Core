<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# RadioSonic USB serial file transfer (`xfer`)

Move files between the board and a host (browser via the **Web Serial API**, or any serial
client) over the **native USB (TinyUSB CDC)** console port — no Wi‑Fi. This is the "both ends"
contract: the firmware side is implemented in [`main/ui/serial_xfer.c`](../main/ui/serial_xfer.c);
this document is what the host/JS side must speak.

## Transport

- The board enumerates as a native USB CDC serial device (TinyUSB on the USB‑OTG controller —
  note this is a *different port* from the USB‑Serial‑JTAG flashing port; the board re-enumerates
  onto it a second or two after boot). The console runs a line‑based REPL with prompt `dsp> `.
  You drive a transfer by writing a command line (ending in `\n`) and then reading/writing the
  framed lines below.
- **Throughput is USB‑bound** (hundreds of KB/s); the nominal baud is irrelevant. Open the port in
  Web Serial with any `baudRate` (e.g. 115200) — it is ignored by the device. 115200, 9600, etc. are
  all equivalent.
- **Flow control must be None.** If a terminal connects but shows no output, it is *not* the baud
  rate. Set Flow control to None (PuTTY: Connection > Serial > Flow control = None). RTS/CTS or
  DTR/DSR flow control can suppress output because the USB‑Serial‑JTAG controller gates on the
  CDC control‑line state. The host should still assert **DTR** on open (standard for Web Serial /
  pyserial) — the firmware uses that DTR edge to detect a (re)connected terminal; it is a one‑shot
  connect signal, not flow control.
- **No reset on connect.** The CDC port does *not* auto‑reset on DTR/RTS, so `port.open()` will
  not reboot the board. (A DTR re‑assert does trigger the console's ~0.5 s terminal re‑probe —
  see Caveats.)
- All frames are ASCII lines terminated by `\n` (the device may emit `\r\n`; strip `\r`). Payload
  bytes are **base64** so the link stays ASCII‑safe regardless of line‑ending handling.
- **Noise tolerance:** other firmware tasks may emit `ESP_LOG` lines onto the same console at any
  time. Every data line is tagged (`D:`); the host MUST ignore any line it doesn't recognize.

## Encoding & integrity

- **base64**: standard alphabet `A–Z a–z 0–9 + /` with `=` padding. Each `D:` line carries ≤ 3072
  raw bytes (≤ 4096 base64 chars).
- **CRC‑32 (zlib/PKZIP)**: poly `0xEDB88320`, init `0xFFFFFFFF`, xor‑out `0xFFFFFFFF`, reflected —
  identical to Python `zlib.crc32` and the common JS table implementations. Computed over the **raw
  (decoded) bytes**, streamed in order. Sent/received as 8 lowercase/uppercase hex digits.

## Commands

### List SD files
```
host →  xfer list\n
dev  ←  F:<name> <size>\n      (repeated, size in bytes, -1 if stat failed)
dev  ←  XFER END\n
```

### Download  (device → host)
```
host →  xfer get sd:<name>\n        # an SD file, sent verbatim
   or   xfer get flash\n            # the flash sample, sent as a complete WAV (44B header + PCM)

dev  ←  XFER BEGIN name=<name> size=<N>\n
dev  ←  D:<base64>\n                (repeated; decode + append + feed CRC)
dev  ←  XFER CRC=<hex8>\n           (CRC-32 of all decoded bytes)
dev  ←  XFER END\n
dev  ←  XFER ERR=<reason>\n         (instead of BEGIN… on failure: nocard|notfound|open|nosample|read|oom|name)
```
Host: on `XFER BEGIN`, read `D:` lines until `XFER CRC=`, decode/concatenate, verify length == `N`
and CRC matches, then expect `XFER END`.

Since 2026-07-06 the `D:` and trailing `CRC`/`END`/`ERR` lines are emitted through the
flow-controlled raw writer (they arrive with bare `\n`, no `\r`) — `get` no longer loses
data to a slow-draining host. Before that fix, any host draining slower than the encoder
lost wire bytes (CRC caught it; transfers could never complete). `getraw` remains the
faster path (no base64 overhead).

### Raw download  (device → host, RAW BINARY payload — preferred for large files)

Both download paths are lossless (see the 2026-07-06 note above). `getraw`'s advantage is
purely **throughput**: no 33% base64 expansion and no per-line framing, so ~900 KB/s of
payload versus ~515 KB/s for `get` — use it for anything bigger than a few MB. SD files
only (the flash sample needs `get flash` for its WAV wrapping).

```
host →  xfer getraw sd:<name>\n
dev  ←  XFER RAWBEGIN name=<name> size=<N>\n    (text; may arrive as \r\n)
dev  ←  <N raw bytes>                           (file contents, verbatim)
dev  ←  \nXFER CRC=<hex8>\n                     (CRC-32 of the N raw bytes; the CRC/END/ERR
dev  ←  XFER END\n                               lines after the payload arrive with BARE \n,
                                                 no \r — they bypass the console's CRLF path)
dev  ←  XFER ERR=<reason>\n                     (instead of RAWBEGIN: nocard|notfound|open|name|oom;
                                                 or after a partial payload: stall|read)
```

Host contract: parse text lines (ignoring unknown ones) until `XFER RAWBEGIN`; every byte
after that line's `\n` is payload — read exactly `N` bytes (large blocks; do not scan),
then return to line mode for the CRC/END lines. Logs are muted device-side during the
payload, so nothing else can appear inside it. If the host stops draining for ~5 s the
device aborts with `XFER ERR=stall`.

### Upload  (host → device)
```
host →  xfer put sd:<name> <size> <crc32hex>\n     # bytes written verbatim to /sdcard/<name>
   or   xfer put flash <size> <crc32hex>\n         # bytes are raw 48k/32-bit/stereo PCM → flash sample

dev  ←  XFER READY\n
host →  D:<base64>\n                (repeated; ≤3072 raw bytes each)
host →  XFER DONE\n
dev  ←  XFER OK\n                   (size + CRC verified)
   or   XFER ERR=<reason>\n         (crc|size|write|open|nocard|busy|b64|timeout|oom|name)
```
Notes:
- `<size>` is the raw byte count (decimal); `<crc32hex>` is the CRC‑32 of those raw bytes.
- The device aborts with `XFER ERR=timeout` if it sees no data for ~30 s.
- On any error for `sd:`, the partial file is deleted. For `flash`, a failed upload leaves whatever
  was received; just retry (a new upload overwrites it).
- **Legal `sd:` names**: no path separators (`/`, `\`), no leading `.`, no quotes or control
  characters — files live flat in the SD root. Anything else is refused with `XFER ERR=name`.

### Live capture stream  (device → host, RAW BINARY — no text preamble)
```
host →  xfer live\n          # uses the current tap   (pre = raw input, post = DSP output)

dev  ←  xfer live\n          # console ECHO of the command (ends in \n)
dev  ←  <44-byte WAV header><raw PCM ...>   ← the response IS the stream: pure bytes, starts at RIFF
host →  <any byte>           # send anything to stop  (closing the port also stops it via TX stall)
dev  ←  \nXFER END\n         # text resync marker, then the dsp> prompt → back in text mode
```
The live stream is **pure binary with no status line** — deliberately, to avoid a text/binary
boundary race in the first USB chunk. The contract:

1. **Send `xfer live\n` and flip to binary mode.** The device echoes the command line (ends in
   `\n`); read and discard up to and including that first `\n`. Everything after it is binary.
   Prefer terminating the command with a single `\n`: with `\r\n`, the console consumes through
   the CR and the orphaned LF used to trip the "any byte stops" abort instantly, killing the
   stream at t=0. Since 2026-07-06 the firmware drains leftover line bytes before arming the
   stop check, so CRLF hosts work — but `\n` remains the documented contract.
2. **The stream starts at the 44‑byte WAV header** (`RIFF....WAVEfmt `), which carries
   `sr=48000 ch=2 bits=32`; read the format from those bytes. Then continuous PCM at the selected
   tap. No base64, no line framing, no CRC (endless stream). The only thing the device emits
   *instead* of the stream is an error line (`XFER ERR=...`), so key "streaming started" on the
   `RIFF` magic.
3. **Stop:** write any one byte. The device finishes the in‑flight chunk, stops, and prints
   `\nXFER END\n` then the `dsp> ` prompt. Because PCM is arbitrary bytes (a literal `XFER END`
   *could* occur in audio), treat the stop as host‑driven: after you send the stop byte, drain and
   discard trailing PCM until you see the `dsp> ` prompt, then you're cleanly back in text mode.
4. **Integrity:** if any I2S RX overrun occurred during the stream (audio lost upstream of the
   device's capture ring — the stream has splice gaps), the device prints one
   `WARNING: <n> I2S RX overrun(s)…` line between `XFER END` and the prompt. Hosts already
   drain to the prompt (step 3), so this is contract‑compatible; a host that wants the
   integrity check can scan that region for `WARNING:`. Absent line = no *upstream* I2S
   loss. Note the live tap is best‑effort by design: if the host drains too slowly the
   device's live cursor skips ahead (a consumer‑lag splice) without a warning line — keep
   the read loop fast (below) and treat this stream as monitoring, not measurement.

Only **one** live reader at a time, and not while a `put flash` upload is in progress
(the upload owns the capture ring; `xfer live` would return an error).

**Host read loop (important):** the firmware can push ~900 KB/s and real‑time audio is 375 KB/s, so
the *transport* has ~2.4× margin — but the host must drain efficiently. Read large blocks
(`reader.read()` into a sized buffer / pyserial `read(65536)`); do **not** do per‑byte work or scan a
growing buffer each chunk, or the host throttles itself and becomes the bottleneck.

### Throughput probe
```
host →  xfer speed [bytes]\n     # default 1048576 (1 MB)

dev  ←  XFER SPEED start bytes=<N>\n   (raw binary follows on THIS port)
dev  ←  <N raw bytes>                  (counter pattern: byte at offset i == i & 0xFF)
dev  ←  \nXFER SPEED done ...\n        (firmware-measured rate), then the prompt
```
Drain the payload exactly like `getraw` (large reads, no per‑byte work); verify the counter
pattern if you want a corruption check. This measures the real end‑to‑end USB rate the other
transfers will see.

## Caveats

- **Light sleep:** while a USB host is attached the board never truly light-sleeps
  (tethered idle keeps the console enumerated), so a sleeping tethered board still
  answers after wake. Untethered light-sleep wake + later plug-in is not yet
  hardware-verified; power-cycle if the port doesn't appear (see `power_sleep.c`).
- **Connect settle:** on DTR assert (port open) the console runs a smart-terminal
  probe for ~0.5 s and may consume bytes sent during that window. **Wait ~1 s after
  opening the port before sending the first command**, or send a bare `\n` first and
  discard until a `dsp> ` prompt appears.
- **One transfer at a time:** drive these sequentially; don't interleave commands mid‑transfer.

## Host reference (Python, stands in for the Web Serial JS)

```python
import serial, base64, zlib

def readline(p):
    return p.readline().decode(errors="replace").rstrip("\r\n")

def download(p, src):                      # src e.g. "sd:long.wav" or "flash"
    p.write(f"xfer get {src}\n".encode())
    data, want, crc = bytearray(), None, None
    while True:
        ln = readline(p)
        if ln.startswith("XFER BEGIN"):    want = int(ln.split("size=")[1])
        elif ln.startswith("D:"):          data += base64.b64decode(ln[2:])
        elif ln.startswith("XFER CRC="):   crc = int(ln.split("=")[1], 16)
        elif ln == "XFER END":             break
        elif ln.startswith("XFER ERR"):    raise IOError(ln)
    assert len(data) == want and zlib.crc32(data) == crc, "integrity"
    return bytes(data)

def upload(p, dst, payload):               # dst e.g. "sd:rt.wav" or "flash"
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    p.write(f"xfer put {dst} {len(payload)} {crc:08x}\n".encode())
    while readline(p) != "XFER READY": pass
    for i in range(0, len(payload), 3072):
        p.write(b"D:" + base64.b64encode(payload[i:i+3072]) + b"\n")
    p.write(b"XFER DONE\n")
    ok = readline(p)
    assert ok == "XFER OK", ok
```

The Web Serial implementation mirrors this over `port.readable`/`port.writable`: split incoming
bytes on `\n`, dispatch by line prefix, `atob`/`btoa` for base64, and a standard CRC‑32 table for
integrity.
