---
name: read-logs
description: Find and read moss's run logs (build/logs/*.log) to see what a window, headless run or server did. Covers tab starts, resizes and exits, agent detection, window attach/detach, D3D11 and HRESULT errors, script lines with their output, profiles, and asserts. Use after a headless run, when checking what happened in the user's last session, when the user quotes an error or reports unexpected behavior, or before guessing at runtime state.
---

# Read the run logs

Every process writes its own log (code/term_debug.c). Paths relative to `moss/`.

## Where
- `build/logs/YYYY-MM-DD_HHMMSS_<mode>_<pid>.log` for a dev build (exe in `build/`); a release build
  writes `%LOCALAPPDATA%\moss\logs`. Modes: `window`, `headless`, `server`, `replay`.
  - Newest of a mode: `ls build/logs/*_server_*.log | tail -1` (names sort by start time).
  - A window and its server run side by side: match them by start time. Headless and `--test` runs
    start their own private server (`instance "-headless-<pid>"` in its header).
- Logs grow while the process runs (shared for reading). Headless runs also print every line to stdout.

## Format
A header: mode, pid, log path, command line, working folder, exe build time and flavor
(`internal` keeps `Assert`), Windows version and core count; `previous <mode> run crashed: <log>` if so.
Then `[frame] source: file(line): message`:
- `[frame]`: the loop's frame (window/headless: the app frame or tick; server: its loop iteration).
- sources: `server` (tabs started/closed/resized, agents found, window attach/drop, vs env),
  `client` (attach, server goodbye), `app` (tab requests, layout restore), `d3d11` (GPU, failures),
  `cells` (terminal font and cell size), `window` (monitor, DPI), `script`/`dump`/`stats` (headless),
  `profile` (timed blocks), `assert`, `image`, `file`.
- Errors carry the HRESULT and its message: `<what> failed: 0x887A0005 ...`.
- Crash sections (`CRASH:` lines without the prefix): see the `debug-crash` skill.

## Rules
- Log per event, never per frame or per item: per-frame cost belongs in timed blocks (`profile-frames`).
- Read the log before guessing what a run did; quote the decisive line when reporting.
