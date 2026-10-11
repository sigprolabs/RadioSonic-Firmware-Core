#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 SigPro Labs, LLC
"""Drive the RadioSonic CLI over the TinyUSB CDC console (pyserial ONLY).

Never use .NET System.IO.Ports.SerialPort against the console — it hangs
(project rule). This tool encodes the hard-won console protocol:

  open -> DTR high -> settle ~3 s (firmware's DTR probe eats the first
  ~500 ms of bytes) -> prime with CRLF until 'dsp>' -> send commands.

Usage:
  python scripts/rs_cli.py [--port COMx] [--settle 3.0] [--cmd-timeout 8]
                           [--banner] CMD [CMD ...]

Commands are sent verbatim. Two pseudo-commands:
  LISTEN:<sec>   passively stream console output (async log lines) for <sec>
  REBOOTCAP      wait for the USB port to drop and re-enumerate (a board
                 reboot), reopen ASAP, stream 20 s of boot output (catches
                 the 'heap: wifi-up/httpd-up' checkpoint lines), then reprime

--banner: print everything received during settle instead of discarding it —
captures the reconnect banner, which includes '[last reset: <reason>]'
(SW/PANIC/POWERON/...). A SW reset after wifi on/ap/<creds> is EXPECTED —
those verbs reboot the board by design.

Examples:
  python scripts/rs_cli.py "ver" "status" "codec"
  python scripts/rs_cli.py --banner "status"          # reset-reason check
  python scripts/rs_cli.py "wifi on" REBOOTCAP "status"   # boot-log capture
  python scripts/rs_cli.py "rec start sd /rec1.wav" LISTEN:10 "rec stop"
"""
import sys, time, argparse
import serial
from serial.tools import list_ports

PROMPT = b"dsp>"


def read_until_prompt(sp, timeout):
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = sp.read(4096)
        if chunk:
            buf += chunk
            if PROMPT in buf[-200:]:
                time.sleep(0.05)
                buf += sp.read(4096)
                break
        else:
            time.sleep(0.02)
    return buf


def listen(sp, secs):
    buf = b""
    deadline = time.time() + secs
    while time.time() < deadline:
        chunk = sp.read(4096)
        if chunk:
            buf += chunk
        else:
            time.sleep(0.05)
    return buf


def prime(sp, tries=5):
    got = b""
    for _ in range(tries):
        sp.write(b"\r\n")
        got = read_until_prompt(sp, 1.5)
        if PROMPT in got:
            return True
    print("!!! no dsp> prompt after priming; last bytes:", got[-300:])
    return False


def wait_port(port, present, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if (port in [p.device for p in list_ports.comports()]) == present:
            return True
        time.sleep(0.2)
    return False


def open_port(port, tries=40):
    for _ in range(tries):
        try:
            sp = serial.Serial(port, 115200, timeout=0.1)
            sp.dtr = True
            return sp
        except serial.SerialException:
            time.sleep(0.25)
    return None


def autodetect_port():
    """Find the RadioSonic console: Espressif VID 0x303A, preferring the
    TinyUSB production console (PID 0x4001) over other states. Returns the
    device name or None."""
    from serial.tools import list_ports
    candidates = [p for p in list_ports.comports() if p.vid == 0x303A]
    for p in candidates:
        if p.pid == 0x4001:
            return p.device
    return candidates[0].device if candidates else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None,
                    help="serial port (default: auto-detect the Espressif VID)")
    ap.add_argument("--settle", type=float, default=3.0)
    ap.add_argument("--cmd-timeout", type=float, default=8.0)
    ap.add_argument("--banner", action="store_true",
                    help="print bytes received during settle (incl. reset-reason banner)")
    ap.add_argument("commands", nargs="+")
    args = ap.parse_args()

    port = args.port or autodetect_port()
    if port is None:
        print("!!! no RadioSonic board found (Espressif VID 0x303A) — pass --port")
        sys.exit(1)

    sp = open_port(port, tries=4)
    if sp is None:
        print(f"!!! cannot open {port} — is another program holding it?")
        sys.exit(1)
    try:
        if args.banner:
            print("===== connect banner =====")
            print(listen(sp, args.settle + 2.0).decode(errors="replace"))
        else:
            time.sleep(args.settle)
            sp.reset_input_buffer()
        if not prime(sp):
            sys.exit(2)

        for cmd in args.commands:
            if cmd.startswith("LISTEN:"):
                secs = float(cmd.split(":", 1)[1])
                print(f"\n===== listen {secs}s =====")
                print(listen(sp, secs).decode(errors="replace"))
                continue
            if cmd == "REBOOTCAP":
                try:
                    sp.close()
                except Exception:
                    pass
                print("\n===== waiting for reboot (port drop + return) =====")
                if not wait_port(port, False, 15):
                    print("!!! port never dropped — board did not reboot")
                    sys.exit(3)
                if not wait_port(port, True, 25):
                    print("!!! port never re-enumerated")
                    sys.exit(4)
                sp = open_port(port)
                if sp is None:
                    print("!!! could not reopen after reboot")
                    sys.exit(5)
                print("===== boot capture (20 s) =====")
                print(listen(sp, 20).decode(errors="replace"))
                if not prime(sp):
                    sys.exit(6)
                continue
            # wifi on/ap/<creds> reboot the board without printing a fresh
            # prompt — the port dying mid-read here is EXPECTED for those;
            # follow them with REBOOTCAP to ride out the reboot.
            try:
                sp.reset_input_buffer()
                sp.write(cmd.encode() + b"\r\n")
                out = read_until_prompt(sp, args.cmd_timeout)
                print(f"\n===== {cmd} =====")
                print(out.decode(errors="replace"))
            except serial.SerialException:
                print(f"\n===== {cmd} =====")
                print("(port dropped during command — board rebooting? "
                      "follow with REBOOTCAP)")
    finally:
        try:
            sp.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
