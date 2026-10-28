---
name: debug-crash
description: Investigate a crash or failed assert of term.exe (window, headless run, or the server). Reads the crash section of the run log in build/logs, opens the minidump in cdb with the symbols the crash handler saved, and finds the cause in the code. Use when a run crashed, a headless script stopped early or exited with an exception code (bash "Segmentation fault", exit 139, 0xC0000005), a log has CRASH or ASSERT FAILED, a server log says "previous server run crashed", or the user says term crashed.
---

# Debug a crash

Crashes are recorded by `WriteCrash` (code/term_debug.c; see "Debugging" in CLAUDE.md). A failed
`Assert`/`AssertAlways` and any fatal exception in term.exe's own code are recorded, also under a
debugger; without a debugger, crashes inside other modules too. The window, headless runs and the
server each do this for themselves. Each crash leaves in `build/logs/` (paths relative to `term/`):
- `<run>.log`: the run log, ending with the crash section:
  - `ASSERT FAILED: file(line): expression` for an assert;
  - `CRASH: <exception> 0x... at <address> (exe+0x...), thread N`, and for an access violation
    `CRASH: read|write|execute of address 0x...`;
  - `CRASH: stack:`, frames symbolized from the PDB (`#00` is the crash site);
  - `symbols:` lines (the copied exe and PDB) and the `minidump:` path.
- `<run>.dmp`: the minidump (internal builds: our heaps and arenas too, ~100 MB; the newest 5 are kept).
- `<run>_symbols/`: copies of term.exe and term.pdb from the moment of the crash. Always use these:
  the next build replaces the ones in `build/`.

Run names sort by time: `YYYY-MM-DD_HHMMSS_<window|headless|server|replay>_<pid>`.

## Steps

1. **Find the run.**
   - Newest crash: `grep -l "^CRASH:" build/logs/*.log | tail -1`.
   - A server crash takes every tab with it; the next server start logs `previous server run crashed: <log>`.
   - No `CRASH:` line means the handler didn't run: killed, crashed before the log opened, stack
     overflow in the handler, or a debugger held the first-chance exception. Reproduce (step 5).

2. **Read the log first.** About 60 lines before `CRASH:`: the last `script:` lines (headless), what
   the server was doing (`tab N started`, `resized`, `window attached`), any HRESULT failures
   (`... failed: 0x...`). For an assert, open its `file(line)` and the callers on the stack first.

3. **Values from the dump** when the log and code don't explain it:
   `.claude/skills/debug-crash/scripts/analyze_dump.sh [DUMP] ["cdb commands"]`
   - No arguments: the newest dump with `.ecxr; kn 30; .frame 0; dv /t /v` (crash context, stack with
     source lines, frame 0's locals). The first run fetches Windows symbols (slow once).
   - Useful: `.ecxr` first always; `kn 30`; `.frame N; dv /t /v`; `dx -r2 Expression` for structs;
     `~*kn 8` every thread; `!address ADDR` what memory an address is in; `lm` modules.
   - Globals: `dx term!g_Debug`, server state lives on ServerMain's stack (`.frame` into it, `dv`).
   - Not in the dump: GPU memory, D3D/COM objects' internals, driver memory.

4. **Find the cause**: compare the values with the invariants in the code and CLAUDE.md.

5. **Reproduce**: rerun the header's command line in its working folder; for a window crash write a
   headless script with the same steps (the `run-headless` skill). Same build + same script ≈ same run
   (terminal output timing is real, so `waitfor` what matters).

6. **Report** the cause with the evidence (log lines, frames, values) and a fix at the right layer.
   Fix only when asked; say when the cause is a guess and what would confirm it.

## Gotchas
- cdb: `cdbX64.exe` from WinDbg (`winget install Microsoft.WinDbg`); the script also tries the SDK's cdb.exe.
- `Unable to verify checksum` warnings are harmless.
- Raw addresses past frame 0: that module's image isn't in the symbols folder (see the `symbols:` lines).
- The `crash` script command (`Debug_Crash`) is inlined: its frame shows as `term!Debug_Crash+0x.. [inlined in ...]`.
