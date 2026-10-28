---
name: profile-frames
description: Profile moss's frames (window, headless, or server loop) - where frame time goes, using TIMED_BLOCK timed blocks collated into "profile:" lines in the run log, GPU timestamp queries, key-to-screen latency hops, and Superluminal captures. Use when something is slow (typing lag, a hitch, heavy output, high CPU while idle), before and after an optimization, or when the user asks where time goes.
---

# Profile frames

Timed blocks (code/term_debug.h) are summed into the current frame of a 240-frame ring per process.
`Debug_LogProfile` writes them as `profile:` lines: work time per frame (waits excluded), the
phases, then each block slowest first (avg/max ms per frame, hits per frame, cycles per hit, file(line)).
Paths relative to `moss/`.

## What triggers a profile
- Automatically: a window frame whose work passes 8 ms (headless: 50 ms; the server loop: 8 ms) writes a
  one-frame profile, up to 5 in a row, then about once a second. Check the log for `slow frame` first.
- Headless: `profile [FRAMES]` in a script (default and max 239).
- Window: Ctrl+Alt+Shift+F10 (the user presses it).

## Steps
1. Build optimized (`build`, the default is /O2 with internal checks; `build release` drops the checks).
2. Write a headless script (`run-headless` skill) that reaches the state, lets it settle, then profiles:
   ```
   waitfor λ
   type type C:\Windows\System32\drivers\etc\hosts
   key Enter
   waitidle
   profile 120
   stats
   ```
3. Read: window/headless phases are `server` (polling the pipe), `app` (UI build), `glyphs` (UI font
   ranges, terminal glyphs and cell resolve), `render`, `present` (headless: waiting for the GPU).
   `stats` adds frames drawn/skipped, glyph atlas use, IPC bytes and GPU ms per pass (chrome before
   the first terminal, the terminal cells, chrome after).
4. Narrow down: wrap suspects in `BEGIN_TIMED_BLOCK(Name)` / `END_TIMED_BLOCK(Name)` (C; END on every
   path out) or `TIMED_BLOCK(Name)` (C++, the rest of the scope). ~30 cycles each, fine in hot code.
5. Compare before/after with the same script and build flavor.

## Typing latency (the ~1 ms goal)
`MOSS_LATENCY=1` makes every process append `<hop> <microseconds>` lines to
`%TEMP%\moss-latency-<pid>.txt` (one clock for all): `key`, `client_send`, `server_in`, `server_write`,
`pty_read`, `server_frame`, `client_cells`, `client_present`. `python tools/latency.py` reports them.

## Superluminal
With Superluminal installed (`C:\Program Files\Superluminal`), every timed block is also a
Superluminal event and threads are named. Capture with Superluminal, then read it with the
`mcp__superluminal__*` tools (open_capture, list_hotspots, find_intervals, list_threads...).
