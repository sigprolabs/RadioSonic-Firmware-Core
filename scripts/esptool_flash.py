# SPDX-License-Identifier: MIT
# Copyright (c) 2026 SigPro Labs, LLC
"""Run esptool, and treat the board rebooting into its new firmware as success.

Usage: python esptool_flash.py <esptool arguments>

A board in ROM USB download mode is restarted after flashing with a watchdog
reset (--after watchdog_reset). The chip drops off USB while esptool is still
finishing that command, so esptool 4.12 reports a pySerial error and exits
non-zero although every segment was written and verified. This script runs
esptool in-process and, when that error comes after the reset has started and
every segment written was verified, reports success instead of the error. Any
other failure is reported as esptool would and exits non-zero.
"""
import re
import sys

import esptool
import serial
from esptool.util import FatalError


class _Tee:
    """Passes esptool's output through and keeps a copy to check afterwards."""

    def __init__(self, out):
        self._out = out
        self.parts = []

    def write(self, s):
        self.parts.append(s)
        return self._out.write(s)

    def __getattr__(self, name):  # flush, isatty, encoding, ...
        return getattr(self._out, name)


def main():
    tee = _Tee(sys.stdout)
    sys.stdout = tee
    try:
        esptool.main(sys.argv[1:])
        return 0
    except serial.SerialException as e:
        text = "".join(tee.parts)
        written = len(re.findall(r"Wrote [0-9]+ bytes", text))
        verified = text.count("Hash of data verified")
        if "Hard resetting with a watchdog" in text and written and verified == written:
            print(f"All {written} segment(s) written and verified; the board is restarting.")
            return 0
        print(f"\nA serial error occurred: {e}")
        return 1
    except FatalError as e:
        print(f"\nA fatal error occurred: {e}")
        return 2
    finally:
        sys.stdout = tee._out


if __name__ == "__main__":
    sys.exit(main())
