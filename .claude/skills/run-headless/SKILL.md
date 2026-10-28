---
name: run-headless
description: Run moss with no window and drive it from a script - type into terminals, press keys, click and drag UI (tabs, splitters, buttons) by label, wait for terminal text, capture PNGs and compare them, dump the pane layout, screens and tabs. Use to check any GUI or terminal change without popping up a window, to write or run the regression sets in tests/, or to reproduce a bug.
---

# Run moss headless

```
cd tests/<set> && ../../build/moss.exe --headless --size 1280x800 --script <set>.txt | cat
```
Options: `--size WxH` (default 1280x800), `--dpi N` (96), `--session FILE` (read and write a layout file;
none by default), `--instance NAME` (a named private server that outlives the run, no tray: session tests).
Headless runs ignore the user's settings.txt. Each run starts its own private server (`MOSS_INSTANCE=headless-<pid>`, ephemeral: no
tray, no tabs.txt) running real cmd + clink tabs; it ends with the run. `MOSS_CLAUDE="cmd.exe /k echo"`
fakes claude. Exit code 0 = no failures, 2 = something failed (timed-out waitfor, label not found, image
different, unknown command). Everything goes to the run log too (`read-logs`).

Ticks are 1/120 s of a fake clock, so animations land on the same frames every run; the server and the
shells run in real time, so wait for what you need with `waitfor`.

## Script (one command per line, `#` comments)
| Command | Does |
|---|---|
| `wait N` | N ticks |
| `sleep MS` | real time, ticking meanwhile |
| `waitfor TEXT [pane=N] [timeout=MS]` | until pane N's terminal shows TEXT (default pane 0, 10 s); logs the screen on timeout |
| `waitidle` | until two ticks in a row draw nothing new and nothing animates |
| `key K` | `Enter`, `Ctrl+Shift+T`, `Alt+Shift+=`, `F11`, `a`: down one tick, up the next (with what Windows would type) |
| `type TEXT` | typed characters, in one tick |
| `cursor X Y` / `cursorby DX DY` | the mouse (drag with a button down) |
| `cursorui LABEL` | the centre of a named UI target or a box with that text (`tab <name>`, `close tab <name>`, `new tab 0`, `splitter <id> left`, `minimize`, `maximize`, `close window`, `palette <command label>`, `close anyway`, `cancel`); logs `cursor: not found` |
| `cursorpane N [COL ROW]` | pane N's terminal centre, or that cell |
| `cursortext TEXT [pane=N]` | the first cell of TEXT on pane N's screen |
| `mods ctrl\|shift\|alt\|none` | modifiers held for the clicks that follow (Ctrl+click) |
| `waitlabel LABEL` | until a UI target with that label exists (a tab's name arriving from its shell; a browser row's file name) |
| `waitgone LABEL` | until no UI target has that label (a deleted file's row) |
| `drop FILE...` | files dropped at the mouse as another program would (paths relative to the run's folder) |
| `lmb\|rmb\|mmb down\|up` | buttons; a widget reacts on the tick after the cursor reached it, so `wait 1` before `lmb down` |
| `wheel N` | wheel notches (positive = up) |
| `capture FILE.png` | the last frame |
| `compare A B [DIFF.png]` | `same`, or `DIFFERENT` with pixel count, max error and rmse |
| `dump layout` / `dump screen [pane=N] [attrs]` / `dump tabs` / `dump commands` | text to the log |
| `cmd NAME [ARGS]` | an app command by name (`dump commands` lists them; `theme Nord`) |
| `dpi N` / `size WxH` | change DPI or the window size |
| `profile [N]` / `stats` | see `profile-frames` |
| `crash` / `crashserver` | a failed assert in the window / the server (tests the crash paths) |
| `quitserver` | the server quits like the tray's Quit |
| `exit` | end |

## Habits
- `waitfor λ` (the default prompt) before typing into a new shell; `waitidle` before every `capture`.
- Find things by label and by pane, never by fixed pixels (except drag distances).
- After a capture, look at the PNG (Read tool) - that's the point.
- Regression sets live in `tests/<set>/` with `run.sh` (out/ vs reference/, `run.sh bless`); out/ and
  reference/ are never committed.
