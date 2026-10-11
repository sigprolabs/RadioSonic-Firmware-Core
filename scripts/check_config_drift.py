#!/usr/bin/env python
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 SigPro Labs, LLC
"""Build-time guard: fail if the local (untracked) sdkconfig has drifted from
the tracked sdkconfig.defaults files.

Why this exists: ESP-IDF applies sdkconfig.defaults ONLY when generating a
missing sdkconfig. An existing checkout never re-syncs, so a defaults change
pulled from git silently does nothing on developers' boards (observed 2026-07-06:
a stale FREERTOS_HZ=100 throttled `xfer live` to ~100 KB/s and looked like a
firmware bug). This script runs on EVERY build via a CMake ALL target, so a
drifted config cannot produce a binary.

Checked sources: sdkconfig.defaults, then sdkconfig.defaults.<IDF_TARGET>
(later overrides earlier — same precedence the IDF build system uses).
Every option pinned there must appear in sdkconfig with the same value;
"# CONFIG_X is not set" pins X off (absent counts as off).

On failure, either fix the value in menuconfig, or delete sdkconfig and run
`idf.py reconfigure` to regenerate it from the defaults.

Caveat: if a defaults entry can never take effect (its Kconfig dependency is
off), a FRESH regen will fail this check too — that means the defaults file
carries a dead entry and should be fixed there.
"""
import os
import re
import sys


def parse(path):
    """Return {CONFIG_NAME: value-string or None-for-'is not set'}."""
    opts = {}
    if not os.path.exists(path):
        return opts
    with open(path, encoding="utf-8") as f:
        for ln in f:
            ln = ln.strip()
            m = re.match(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$", ln)
            if m:
                opts[m.group(1)] = m.group(2)
                continue
            m = re.match(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$", ln)
            if m:
                opts[m.group(1)] = None
    return opts


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    sdk = parse(os.path.join(root, "sdkconfig"))
    if not sdk:
        print("[config-drift] no sdkconfig yet (fresh configure) — nothing to check")
        return 0

    target = (sdk.get("CONFIG_IDF_TARGET") or '""').strip('"')
    sources = ["sdkconfig.defaults"]
    if target and os.path.exists(os.path.join(root, f"sdkconfig.defaults.{target}")):
        sources.append(f"sdkconfig.defaults.{target}")

    pinned = {}
    for s in sources:
        pinned.update(parse(os.path.join(root, s)))

    drifted = []
    for key, want in pinned.items():
        have = sdk.get(key)  # absent == not set == None
        if want != have:
            drifted.append((key, want, have))

    if drifted:
        print("[config-drift] FAIL — local sdkconfig has drifted from " + " + ".join(sources) + ":")
        for key, want, have in drifted:
            show = lambda v: "(not set)" if v is None else v
            print(f"  {key}: defaults={show(want)}  sdkconfig={show(have)}")
        print("[config-drift] Fix the value(s) in menuconfig, or regenerate from defaults:")
        print("[config-drift]   del sdkconfig && idf.py reconfigure")
        return 1

    print(f"[config-drift] OK — {len(pinned)} pinned options match ({' + '.join(sources)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
