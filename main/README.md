<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 SigPro Labs, LLC -->

# main/ — composition root and shared contracts

Everything with behavior lives in `audio/`, `board/`, or `ui/`; this level
composes them and defines what they share.

- `radiosonic_main.c` — `app_main` boot order, the three core-1 audio tasks +
  ping-pong buffers, I2S init, and the authoritative task/core/priority map.
- `common.h` — pure data contract: audio constants, runtime geometry
  `g_geom`, shared state `g_state`. No functions besides inline helpers.
- `audio_pipeline.h` — the control surface (suspend/resume, heap checkpoint,
  overrun counter) other modules use to talk to the pipeline.

Dependency rule: `main` includes everything; nothing includes back into it.
The full picture is [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md).
