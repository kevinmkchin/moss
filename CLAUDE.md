# CLAUDE.md — moss project

Architecture, rules and gotchas for moss (called term while it was being made). This complements the
global `~/.claude/CLAUDE.md` (functional-first, simplicity, ZII, no helper until its second use, side effects
at the edges); those rules apply here, match them.

Also in the repo: `docs/techniques.md` (performance, architecture and UX techniques worth knowing before changing
the frame loop, renderer, UI, panes, palette or file browser; ideas only, in our own words) and `wishlist.txt`
(features the user wants next).

## What this is

A Windows terminal meant to beat Warp: more performant, more powerful, more fun. Its own GUI (immediate-mode UI
laid out with rect cuts, one SDF quad shader, panes with tab strips; all written for moss) on clod's terminal core
(`C:\UnitySrc\w\clod`: background server, ConPTY, emulator, cmd + clink shell, agent detection). "Bring your own
CLI agent": Claude Code or any other agent CLI (codex, gemini, aider, opencode, ...), Claude with the deepest
integration. Open source, GPL-3.0 (`LICENSE`), at github.com/kevinmkchin/moss.

Design: moss's own (not Windows 95). Scope for now: the shell and a file browser. Every tab has two faces, its
shell and a browser of the shell's folder, flipped with a key; browsing cd's the shell there when it's idle at
its prompt. While the tab does work (a program running, or an agent session open even if idle) the browser is
read-only: you can browse anywhere (the shell doesn't follow) and look, preview, copy out; nothing that changes
the disk.

## Website content (noclip.gd/moss)

`build content` builds moss, then writes the landing page's content to `content\`: three screenshots of the fresh
build (`tools\site\capture.py`: headless, with made-up sessions, a fake claude and fake projects at `C:\src`, nothing
of the user's), the icon, and `content.json` with the tagline, install line and features below (`tools\site\content.py`).
The site generator (`C:\UnitySrc\kevin.gd\build.py`) copies that folder into the page. The page should feel zen:
quiet, short, nothing loud.

Tagline: moss is a terminal and file explorer for Windows
Install: winget install moss

Main features, in order. Short phrases, no descriptions, only what's special about moss:

- faster than Windows Terminal
- 1 ms from keypress to screen
- switch between terminal and file explorer in every tab
- split panes, tabs you drag between them
- tabs keep running in the background
- tabs survive restarts and reboots
- Claude Code and other CLI agents, every session in a sidebar
- folder sizes, measured in the background
- tab completion and autosuggestions
- unix tools from Git for Windows
- search the scrollback

## Processes and files

- `moss.exe` is everything (`code/term_win32.c` `wWinMain`): the window, `--server`, `--headless`, `--vt-replay`.
- `moss --server` (hidden): owns the tabs (`code/term_server.cpp`, from clod's server): ConPTY children,
  `vt_terminal` emulators (`term_vt.cpp`), scrollback, agent detection, `tabs.txt`, shell history, the tray.
  It outlives the window. The first window starts it (`ConnectToServer`).
- Tabs' console host is the bundled OpenConsole (`vendor/conpty`, MIT, Windows Terminal's:
  `conpty.dll` loads `OpenConsole.exe` from next to `moss.exe`; `Conpty()` in `term_pty.cpp`), not the system
  conhost, which was the throughput limit (64 MiB `type` in a pane: 3.1 s with conhost, 0.45 s with OpenConsole; Windows Terminal 0.67 s;
  `tools/bench`). `MOSS_CONPTY=system` goes back to conhost. OpenConsole doesn't repaint on resize: it asks where
  the cursor went (`ESC [ 6n`) and expects the terminal to have reflowed, so `VtResize` reflows the main screen
  when the width changes (the scrollback stays as it was). `MOSS_PTY_CAPTURE=<folder>` writes each tab's raw
  console host output to `<folder>\pty-<thread>.bin` (to compare the hosts' VT).
- The window (`moss.exe`) owns the layout and draws. Two unity builds:
  - C (`term_c.c`): `term_core` (arenas, strings, UTF-8, hashing), `term_draw` (the DrawList), `term_ui`
    (hit areas, signals, animations, text edit), `term_files_win32.c` (folder lister and watcher threads),
    `term_debug.c`, `term_panes.c` (pane tree, session text), `term_app.c` (the app, pure), `term_win32.c`
    (platform + headless runner).
  - C++ (`term_cxx.cpp`): the server, `term_client.cpp` (pipe client: tab list, grids), `term_cells.cpp`
    (terminal cells on the GPU), `term_gpu.cpp` (D3D11: the quad shader, composition swap chain, headless
    texture), `term_font.cpp` (UI glyphs, DirectWrite), WIC images.
  - The boundary is `term_platform.h` (C API to the C++ side) and `term_types.h` (what the app sees of tabs).
  - Unity builds share one namespace: a static name can clash with another file's or with Win32's (`DrawIcon`,
    `Replace`); the build error says so.
- The app (`term_app.c`) is pure: `App_Frame(AppFrameInput) -> AppOutput + DrawList`. Every side effect
  (server requests, clipboard, window commands, session file, settings) goes through `AppOutput`.
  Keep OS calls out of it.
- Protocol (`term_ipc_proto.h`): per tab, not a composed screen. Server -> window: `MSG_TABS` (the list, on
  change), `MSG_CELLS` (changed rows of shown tabs, cursor, modes; at most one frame per 8 ms, held up to 100 ms
  for a synchronized update), `MSG_OUTPUT` (OSC 52, bell). Window -> server: `MSG_WATCH` (every tab the panes
  have, its pane's size in cells, shown or not; sent once the layout is still), `MSG_INPUT`, `MSG_NEW_TAB`,
  `MSG_CLOSE_TAB`, `MSG_SCROLL`, `MSG_FOCUS`, `MSG_DEBUG`, `MSG_SET_OWNER`, `MSG_FIND`. **Bump `TERM_PROTOCOL_VERSION` when a message's
  layout or meaning changes.**
- One main window: a second `moss` brings the first one up and exits (mutex `Local\moss-window`, the window's
  `moss.main` property; not for `--test`, `--instance` or headless runs). The server says goodbye with a reason
  and a BYE_* byte: tray Quit (or another window of the same name taking over) closes the window instead of it
  reconnecting and starting a new server.
- More windows (tear-off): each is its own process, `moss --window NAME [--at x,y,w,h]` (mutex
  `Local\moss-window-NAME`, layout in `window-NAME.txt`), and the server serves them all at once (`client`, one
  per pipe connection; a tab streams to the window showing it, `tab::Viewer`). Every tab belongs to one window,
  `tab::Owner` (its name; empty = the main window), kept in tabs.txt and sent in `MSG_TABS`; a window shows only
  its own tabs (the app's reconcile) and `MSG_SET_OWNER` moves one. A tab dragged out of the window
  (`AppOutput.tear_off_*`) goes to the moss window under the mouse (same server: the `moss.id` property, an atom
  "<instance>|<name>"), else to a new window there (not the window's only tab); `tear_off` in the palette does
  the same. A window the user closes hands its tabs back to the main window and deletes its layout; one closed by
  tray Quit or logoff keeps them, and the main window reopens it at its next start (`open_windows`: owners with
  no window holding the mutex). Tests: `tests/windows` (three headless runs on one named server).
- Dragging a tab across to another moss window: while the pointer is outside its window, the dragging window tells the
  one under it where the pointer is (`SendDrag`: WM_COPYDATA 0x7E4E, over / gone / dropped, its label; `AppOutput.drag_out`),
  and that window draws its own drop buttons and strip gap for it (`AppFrameInput.foreign_*`: the same code as a tab of
  its own, no tab lifted). A drop lands where it was dropped: a server tab once the server hands it over (`App.arrive_tab`,
  `DropTabAt`), a web tab's page opens there again. Test windows torn off a test window stay test windows (`--at`), so
  two can be driven with posted messages.
- The window keeps its layout in `%LOCALAPPDATA%\moss\layout.txt` (`Session_Write`/`Session_Read` in
  `term_panes.c`: "term-layout 1", a `window` line, then a `split`/`pane` line per node with its depth and a `tab`
  line per tab; a file it can't read is ignored and the window starts with one pane): pane tabs
  find their server tabs again by lasting id (claude session id, else the shell's history id). If the server
  goes away (quit, crash) the window keeps its panes, says so, reconnects every 2 s, and re-matches by id.

## Rendering

- One D3D11 device (`term_gpu.cpp`, BGRA support for Direct2D). Everything on screen is one instanced `Quad`
  (`term_draw.h`: rounded rect with border, dashes and soft edge by SDF, a glyph from the R8 atlas, or an image),
  batched by clip and texture, except terminals: `Draw_External` puts a `TermCellsDraw` batch in the list, and
  `Gpu_Render` calls `TermCells_Draw` in its place, scissored to the pane, then rebinds its own pipeline.
- The app draws in painter's order (later on top) while laying out with rect cuts (`CutLeft` & co.). Hover comes
  from last frame's hit areas: the last one registered under the mouse wins, so register a thing's hit area where
  it's drawn (a popup's `Blocker` first keeps everything under it from the mouse). Splitters register twice: before
  the panes (their drag moves the panes this frame) and after (they win the hover).
- `TermCells_Prepare` runs before rendering (Direct2D can't draw glyphs inside the backend's pass): each shown
  tab's grid is resolved into GPU cells (palette colors, glyph tiles drawn on demand with DirectWrite: font
  fallback, color emoji, clusters across cells) only when its version, palette or font changed. Selection and
  cursor are shader constants, not baked.
- Startup: the window is created hidden and shown after two frames, cloaked (`DWMWA_CLOAK`) while it's shown and
  one more frame is drawn at its final size, so the first thing on screen is a finished frame; the log says
  "shown N ms after start".
- Popups ease in (`PopupAppear`), themes fade (`theme_from`), tooltips show at once for a moment after one was on
  screen (warm), the palette ranks by `FuzzyScore` (characters in order; word starts, runs and early matches score
  higher).
- A lost GPU device (a display driver installed or restarted, e.g. by Windows Update during a `winget upgrade`; a GPU
  reset) leaves nothing drawn showing, so after each present the window checks (`Gpu_LostReason`) and, once it's
  gone, saves its layout, exits and starts the same window again (`--after <pid>`: it waits for this process, and its
  mutex, to go; the tabs stay with the server, web pages load again). `MOSS_TEST_GPU_LOST=N` fakes it from frame N.
- Frames are drawn on events only; unchanged frames (draw list hash) aren't presented; presents are paced to
  the refresh rate (6 ms apart at most) but a frame after a quiet spell goes out at once.
- Opacity (`opacity` in settings.txt): the swap chain goes through DirectComposition with premultiplied alpha
  (window `WS_EX_NOREDIRECTIONBITMAP`); only terminals' default backgrounds are see-through.
- Tab drags show a drop overlay like File Pilot's (its reference picture: the user's `layouts.png`): a row in the pane under the pointer (left / join / right; top / join / bottom in a side-by-side
  group), the window's edges across the root's split (a single pane: top and bottom), and the borders of every group
  around the pane. Only a button takes a drop (over a tab strip, the strip's gap does). Buttons are dark with a dashed
  edge; inside, the panes are outlined cells and the new one is in the accent (brighter under the pointer); where the
  tab lands gets a quiet accent wash growing from there; a card by the pointer names the tab. The drop is worked out
  before the panes are drawn (a strip opens its gap), the overlay drawn after them.
- UI text (`term_font.cpp`): Segoe UI and Segoe Fluent Icons (Windows 10: Segoe MDL2 Assets), each glyph drawn
  with DirectWrite into one 2048² atlas on first use; a codepoint the font lacks comes from a few system fallbacks
  (Segoe UI Symbol, Emoji, the CJK UI fonts). The busy spinner is ◐◓◑◒ (no rotated glyphs). Tab names drop the
  marks programs put in front (`TabLabel`).
- Moss (green-black, lime accent) is moss's official theme: first in `g_TermThemes`, so the default, and the
  one for screenshots, the website and the icon. Graphite, Dusk, Signal, Paper (moss's own) and a few borrowed
  ones are the other choices.
- Chrome colors come from the terminal theme (`ChromeFor`): panes are its background, the title band a shade off
  it, popups a shade towards the text; hover and press are see-through text color.

## Input

- Bound commands first (`g_Commands` in `term_app.c`; Windows Terminal-like defaults: Ctrl+Shift+T/W/A/P,
  Alt+Shift+=/-, Alt+arrows, Ctrl+Tab, Ctrl+=/-/0, F11, Shift+PgUp/PgDn), then the focused terminal: special
  keys as VT sequences (`SpecialKey`), the rest as typed text (WM_CHAR kept with its key; Alt+key is ESC +
  text; Ctrl+letter arrives as its control character).
- Ctrl+Shift+Up / Ctrl+Shift+Down put the previous / next command's prompt at the top (`SCROLL_PREV_MARK` /
  `SCROLL_NEXT_MARK`): each OSC 9;9 from the prompt marks its line (`vt_terminal::Marks`, absolute lines:
  ScrollbackPushed + row, and the folder; dropped when the screen or scrollback is erased). ConPTY passes the OSC
  through out of step with the text around it (lines off), so a jump first finds each mark's real row: the one
  within 30 lines that starts with its folder (the prompt's first line), then keeps it (`mark::Exact`).
- Mouse in a terminal: reports to a program that asked (SGR or X10; Shift keeps the mouse for selecting),
  else drag selects (double-click word, triple-click row, copy on select), Ctrl+click opens http(s) links,
  right-click pastes, the wheel scrolls back (arrows in an alt screen).

## Tabs and panes: zoom, names, the palette

- Ctrl+Shift+Z (`zoom_pane`) shows the focused pane over the whole area (`App.zoom_node`; its strip gets a "back"
  button); the other panes wait behind it, watched at their own sizes. Focusing another pane, or the pane going,
  ends it.
- A strip with more tabs than fit at their narrowest scrolls, as Sublime's does (`PaneNode.strip_scroll`): the wheel
  over it (either way), a reorder drag held near an end; a tab newly shown is scrolled into view (`strip_seen`), the
  new tab button stays at the end, the ends fade where tabs are scrolled away, and tabs scrolled out take no clicks.
- "Rename tab" (palette) or a double-click on a tab edits its name in the strip (Enter keeps, Esc drops, empty =
  the server's name again): `PaneTab.title`, kept in layout.txt (`Title`), used by the strip, the window title,
  the palette and layout dumps.
- The palette lists the tabs whose name or folder has the typed text before the commands; Enter goes to the tab.

## Terminal conveniences

- Scrollbar: a thin thumb at a terminal's right edge while scrolled back, with the mouse near it, or dragged
  (built before the terminal so it wins the hover; `App.sb_*` keeps the offset asked for while the grid catches up).
- Summon key: the main window registers settings `summon` (default Win+\`; `off` = none) with `RegisterHotKey`; it
  brings the window up from anywhere, or minimizes it when it's in front.
- Key bindings: settings.txt `key.<command> = <keys>` ("ctrl+shift+d", "none"; names as `dump commands` lists
  them) override `g_Commands`' keys (`App.bind_key/mods`, read from `AppInit.settings_text`; `g_KeyNames`).
- Ctrl+Shift+K clears the scrollback (`SCROLL_CLEAR`); the screen stays.
- Settings: Cursor (block, bar, underline: when the program asks for none, `TermCellsDraw.cursor_shape`), Cursor
  blinks, Copy on select. "Type into every pane" (palette, `App.broadcast`): keys go to every terminal shown in the
  window, each edged in the accent.
- A right-click on a tab strip's empty part opens the settings (the platform swallows the system menu on caption
  right-clicks, `WM_NCRBUTTONUP`). Right-click on a tab: rename, a color (the theme's red to cyan, a line along the tab's top, kept in layout.txt
  `Color`), move to a new window, close the tabs to the right / the others, close.
- Paste (Ctrl+Shift+V, right-click) goes through the app: it asks for the clipboard (`read_clipboard`), types it
  (`PasteBytes`: line ends as Enter, bracketed when the program asked), and asks first ("Paste N lines?",
  `ConfirmBox`) when several lines would go to a program that didn't ask for bracketed paste (settings
  `paste_warning = off`). Headless runs have their own clipboard (`clipboard TEXT` in a script, `
` for line breaks).

## From outside: `moss -d DIR [-- command]`, Explorer

- `moss -d DIR` (or `--cwd`) opens a tab there (`-- command line` is ignored for now: no profiles). With a main
  window running, the new process hands it over (`WM_COPYDATA`, cwd   command, UTF-8) and exits; otherwise the
  tab opens once the server's tabs are in (`AppFrameInput.open_request` until `open_taken`; the first-tab rule waits).
- Settings "\"Open in moss\" in Explorer" registers (HKCU `Software\Classes\Directory[\Background]\shell	erm`,
  `moss.exe -d "%1"` / `"%V"`) or removes it; its state is read from the registry at start.
- Alt+Shift+arrows move the focused pane's edge (the nearest split that way) by 5%. Ctrl over a web address
  underlines it (`TermPalette.underline_*`).

## Selection, copying, ligatures

- Alt+drag selects a rectangle (`Selection.block`, `TermCellsDraw.sel_block`, the shader's `SelBlock`).
- Rows whose line went on in the next carry `CELL_WRAPPED` on their last cell: copying (a selection, "Copy
  everything") joins them into one line, and a web address that wraps opens whole.
- Settings "Ligatures" (`ligatures = on`, `TermPalette.ligatures`): up to four programming symbols of the same look
  go to DirectWrite as one run across their cells, so a font's ligatures show (Cascadia Code).

## Keys to console programs: win32-input-mode

- ConPTY asks every tab for win32-input-mode (`?9001`, `vt_terminal::Win32Input`, `CELLS_WIN32_INPUT`). While on
  (settings `win32_input = off` turns it off), each key goes as the console key event it is,
  `ESC [ Vk;Sc;Uc;Kd;Cs;Rc _`, presses and releases (not the release of a key moss kept for a command,
  `App.vk_withheld`), so PSReadLine, far and vim see Ctrl+Backspace, Ctrl+Space and the rest as pressed. Text with no
  key (an IME's) and Shift/Alt+Enter's ESC CR for agents go as characters (Vk 0), pressed and let go.
- `UI_KeyEvent.vk/scan/control_state` come from the platform; headless runs give their keys the real Windows key.

## Backdrop

- Settings `backdrop` (`none`, `acrylic`, `mica`; the settings' "Behind the window"): what Windows 11 draws behind
  the see-through parts (terminal backgrounds below 100% opacity). The platform extends the frame over the whole
  window (`DwmExtendFrameIntoClientArea` -1) and sets `DWMWA_SYSTEMBACKDROP_TYPE`, plus the frame's dark mode from
  the theme (`AppOutput.backdrop`, `dark`). Picking one at 100% opacity sets 80%. PrintWindow captures don't show
  it; check on screen.
- `AppOutput.setting_keys/values`: up to 4 settings.txt writes a frame (`SetSetting`).

## Links

- Ctrl+click in a terminal: an OSC 8 hyperlink (the emulator keeps each link's URI by id, `vt_terminal::Links`, and
  marks cells with it in `cell::Flags >> CELL_LINK_SHIFT`; the window asks with `MSG_LINK_GET`, the server answers
  `MSG_LINK`), else the word under the pointer: http(s) opens in the browser, anything else as a path relative to the
  tab's folder (`AppOutput.open_path`). `OpenLink` (term_win32.c) decides: a file with `:line:col` or `(line,col)`
  goes to settings.txt `editor` when set (`{file}` `{line}` `{col}`, else "path:line:col" appended), else its own
  program; programs and scripts are only shown in Explorer, never run; a folder opens in Explorer; a path that isn't
  there does nothing. Ctrl over an OSC 8 link underlines it (`TermPalette.link_hover`) with a hand cursor.
- Headless runs log what they would open; `run_set.sh` keeps those lines in dumps.txt (`tests/links`).

## Search (Ctrl+F, or Ctrl+/ as vim's /)

- The bar at the top right of the focused terminal's pane has the keyboard: typing goes to the newest match.
  Ctrl+F's bar (`App.find_vim` off): Enter steps to the next (older) match, Shift+Enter back, the bar keeps the
  keyboard. Ctrl+/'s: Enter keeps the match (the bar stays), then n older, N newer, / types again; any other key
  closes it and goes to the program. Esc closes either. Up / Down step while typing. Smartcase: an uppercase letter matches case. A click in the pane takes the
  keyboard from it. In a browser, typing filters already.
- A shell's search is the scrollback: the server finds (`MSG_FIND`: tab, `FIND_*`, text; `MSG_FIND_STATE`: count,
  current): every match in the scrollback and screen, the current one stepped and scrolled into view. `EncodeTabCells`
  marks matches in the cells it sends (`CELL_FOUND`, `CELL_FOUND_CURRENT`), ticks show where they are.
- A claude tab (`TermTab_Agent` with a session key) searches its whole history instead: `MSG_HISTORY_GET` makes the
  server read the transcript (`ReadHistory` in term_sessions.cpp, its own thread: the last 16 MB, prompts and
  claude's text, at most 2000 messages of 64 KB) and send it (`MSG_HISTORY`). The history view covers the terminal:
  messages wrapped to the pane (`App.hist_lines`, rebuilt when the history, width or text change), matches in the
  match color, the current one framed and scrolled to the middle.
## Claude tabs made comfortable: the conversation, the composer, clean copy

- Chat mode: a claude tab shows its conversation (`Chat`, one per claude tab shown lately, `App.chats[8]`): the
  transcript as an article above a prompt, not claude's TUI. Settings "Claude tabs in chat mode" (`claude_chat = off`)
  turns that the other way; Ctrl+Shift+R (`chat_mode`) flips one tab (`PaneTab.chat_flip`). Only while claude sits at its own
  prompt box (`ClaudePromptBox`: a rule, the `❯` line (a no-break space after it), a rule, the lowest rule within the
  bottom 12 rows written, nothing under it but the mode line and notices such as a usage warning: no other rule, no
  `❯ 1.` choice): a question, a permission, a menu (/model) or the start shows the terminal, and the conversation comes back
  once it's answered there.
- Its prompt (`Chat.draft`, a `Ui_TextArea`) is written on the page itself: the column's width and reading type
  (`Font_Read`), under a hairline (darker while it has the keyboard), no box; under it the keys, faint, and "Send ↵" /
  "Stop · Esc" as a word in the accent. Enter sends (`ChatSend`: as typed, or a
  bracketed paste when it has lines; Enter goes 120 ms later on its own, as the key, so claude doesn't take it as part
  of a paste: `SendLine`, `App.enters`, also the model picker's `/model` and the composer; an Enter still on its way to
  the tab goes before the next line), Shift+Enter is a new line, Esc stops a turn (win32-input-mode's Esc key
  when asked), Shift+Tab claude's mode, PgUp/PgDn and Ctrl+Home/End scroll, Ctrl+V a picture's path, files dropped and
  Ctrl+Shift+V pastes go in. The prompt shows at once (`Chat.pending`) until the transcript has it (20 s at most; never
  for /commands). While claude works (its title's spinner, or "esc to interrupt" on its mode line: `ClaudeWorking`): a
  ◐◓◑◒ spinner and claude's own status line (the row over its box, its mark dropped; else "Working…") above the box, a
  stop button. The wheel scrolls anywhere over the view (rows and code blocks take the hover).
- Live: the window asks for the history (`AppOutput.history_asks`: tab, from, have) when it shows, every 400 ms while
  a turn runs or 4 s after a send, a change or the turn's end (its last reply can land in the transcript a moment
  after claude's screen says it's done), else every 5 s. The server keeps each tab's transcript read
  so far (`history_cache` in term_sessions.cpp: `ReadHistoryMore` reads only what was written since, the first time
  the last 16 MB; another session or a shorter file starts over; versions count server-wide from the clock) and
  answers `MSG_HISTORY` with the messages from `from` on, or nothing if unchanged (protocol 18; 19: kinds 3 and 4). `Stable`: the messages
  before it won't change (the last may go on, a tool waits for its result). Its size comes from an open handle (the
  folder entry lags while claude keeps the file open). Lines starting `{"type":"api-request` (Claude Code 2.1's copies
  of what it sent) are skipped, a tool id written twice counts once.
- The app keeps the messages (`Chat.msgs`, text in `msg_arena`; a reply replaces from its `from` on) and builds lines
  only from the first message that changed (`ChatRebuild`, `Chat.built_at`: each message's first line, block and text
  position; reused line slots are zeroed); a width change builds all (a 100 MB transcript's last 16 MB: 20 ms, once).
  A view at its end stays there as lines come (`at_end`, `scroll_seen`) until scrolled up.

- The composer (Ctrl+Shift+I, `compose`; any terminal tab): a panel over the bottom of the pane with a real text box
  (`Ui_TextArea` in term_ui.c: wrapped lines, Up/Down, mouse, a double click's word, undo in bursts, 16 KB; laid out
  only while open). Enter is a new line, Ctrl+Enter sends (`PasteBytes`, bracketed when the program asked, then `\r`),
  Esc puts it away (one draft, `App.compose`, kept). Ctrl+V pastes text, and a picture on the clipboard (no text) is
  saved as `%TEMP%\moss-paste\paste-<time>.png` (`SaveClipboardImage`, `AppOutput.save_clipboard_image` ->
  `AppFrameInput.pasted_image`) and its path inserted (claude reads pictures by path); files dropped on the pane go in
  as paths. While open on the focused tab it takes every key the commands leave.
- Its markdown: headings, **bold** and `code` runs (`HistSpan`), list items (marker, indented lines), fenced code
  (`Font_Mono`: Cascadia Mono, else Consolas; a shade; the block's copy button, `Chat.blocks`), tables (columns as wide
  as their widest cell, narrowed together to fit), rules. Ctrl+F searches it (a claude tab whose terminal shows gets the
  conversation while searching).
- It reads like an article in a magazine (The New Yorker's look): one centered column (about 70 characters, `R(64)` at
  12 pt), Libre Caslon Text to read (`Font_Read`, `Font_ReadBold`), Libre Caslon Display headings (`Font_ReadHead`), the
  user's prompts in Segoe UI (`Font_ReadSans`) in a soft rounded bubble, Cascadia Mono code. moss ships the Caslons
  (`vendor\fonts`, SIL OFL, copied to `fonts\` next to moss.exe; Georgia when they're missing): `Font_Create` takes a
  font file's full path as a family. Caslon's hairlines are thinner than a pixel at reading sizes, so the reader's fonts
  darken their coverage as DirectWrite's enhanced contrast does (`Font.contrast` 1.5: a' = a(k+1) / (ak+1)). Its text size is its own (settings `reader_size`, "Chat mode > Text size"; Ctrl+= /
  Ctrl+- / Ctrl+0 in chat mode, `App.reader_pt` -> `AppOutput.reader_pt`: the platform makes the reader's fonts
  again, and the lines are laid out again once the new fonts are in). Its colors are its own too (settings
  `reader_theme`, "Chat mode > Colors", `ReaderTheme_*`): Paper (warm white, ink, an oxblood accent; the default), Evening
  (the same at night) or the window's theme; the pane's account line and strip stay the window's; each line has its own height and place
  (`HistLine.y/h`: gaps before a message, a heading, a block); headers in local time ("Today 14:03"); code blocks in a
  container with a header (the fence's language, the copy button); tables padded, the header row on a shade.
- Notes (`MSG_HISTORY` kind 3, `HistLine_Note`: small, dim, a hairline at the left): a /command and its output (system
  `local_command` lines, or user lines wrapping `<command-name>` / `<local-command-stdout>`; colors and tags out),
  notices (`informational`: usage limit reset, unknown command), "Conversation compacted", "Interrupted". A shell command
  typed after `!` (`<bash-input>`, `<bash-stdout>`) is a tool row named `!`. A prompt sent as blocks keeps its text
  (`[Image]` for a picture).
- Tools used in a row (no text between) fold into one row, as Claude Desktop's ("Ran 3 commands, read 2 files (1
  failed)": `HistLine_Group`, by kind in order of first use); open, each tool's row under it, indented. Unlike Desktop's
  (anthropics/claude-code#90425) text between tools is never folded. A rebuild inside a run starts at the run.
  A failed tool (`is_error`: kind 4) has its name in red. A result's keys come in any order (Claude Code writes
  `content` before `tool_use_id`): each `tool_result` block is read from its `{` to the next one's.
- The tools claude used are in it too: the history read keeps each `tool_use` (its name, a summary: the command, the
  file, the pattern, ...; its detail: the whole command, an edit as '-' / '+' lines, a file written) with its
  `tool_result` (`MSG_HISTORY` entry kind 2, fields split by \x1f). Each is a row (a chevron, its name, the summary in
  mono); a click opens it (`Chat.open` by key; its message is built again): the detail (an edit's
  lines green and red) and the output under a rule, at most 400 lines.
- Copying from an agent's tab (a selection, copy on select; not a rectangle) is cleaned (`CleanAgentCopy`): claude's ●
  and ⎿ marks become spaces, the indent the lines share goes, a line wrapped near the screen's edge joins the next.

## A claude tab's account line

- Under a claude tab's terminal, a row says who's billed and whose email it reports with, as its /status would: login
  method, organization, email, model (`msg_tab` account string, `TermTab.account`; its terminal is a row or so
  shorter). The scan thread reads it for each claude it finds (`ClaudeAccount`): the claude's own environment
  (`ProcessEnvironment`: Bedrock / Vertex / Foundry, an auth token, an approved `ANTHROPIC_API_KEY`, and
  `CLAUDE_CONFIG_DIR`, which picks the account), `.claude.json`'s `oauthAccount`, `.credentials.json`'s
  `subscriptionType` (nothing else is taken from it; secrets read on the way are wiped), and the model of the
  transcript's last reply (else `--model`, `ANTHROPIC_MODEL`, settings.json, "Default"). Organization and email only
  for a claude.ai login. Test: `tests/sessions` (a made-up Max login).
- Its 5th field is the context's size: the last reply's usage (input, cache written, cache read) in the transcript tail;
  the hold compares login, organization and email only. On the line's right: "Mode" (claude's permission mode read off
  its screen's bottom 4 rows: plan, accept edits, auto, bypass permissions, else default; a click sends Shift+Tab) and
  "Context 42k". The model is a button: the palette's `PaletteMode_Model` (`g_Models`, or a name typed) types
  `/model <name>` and Enter, not while a turn runs or the tab is held. Test: `tests/claude_ui`.
- Another account's claude (its own `CLAUDE_CONFIG_DIR`, e.g. set in the shell) registers in its own config folder:
  the scan looks there too (the process's environment, `EnvVar`) and keeps it in `tab::ConfigDir`. tabs.txt saves it
  (the 7th field), a restart resumes the claude under it (the tab's environment gets that `CLAUDE_CONFIG_DIR`), and the
  history search and the wait for a previous server's claudes look in it.
- A login, organization or email that changes under a running claude (a /login elsewhere, files rewritten) holds it so
  it can't go on billing the new account unasked (`tab::AccountWas`, `TAB_ACCOUNT_HELD`): the server stops its turn
  (Esc, when busy or asking), drops every `MSG_INPUT` to it, gives the tab a notice and a tray notification. The pane
  shows a card (was / now) whose "Go on with the new account" sends `MSG_ACCOUNT_OK`; the account changing back ends
  it too. A model change is only shown. A config file caught mid-write (not whole JSON) is unknown, never a change.
  The tab the change came from isn't held: the server follows each claude's prompt line as typed (`tab::TypedLine`,
  win32-input-mode key events decoded), and a submitted line starting `/log` (/login, /logout) makes an account change
  within 10 minutes that tab's own (`tab::LoginAtMs`); the other claudes on the same config folder are still held.

## Claude accounts and work folders — the billing safety net, don't weaken it

- The user bills a personal account and an Enterprise one; Enterprise must only ever be billed for work, which lives
  under `C:\dev`. `~\.moss\accounts.txt` (`MOSS_ACCOUNTS_FILE` in tests; headless and test runs get `NUL`, none) is the
  one source the server, the window and clink read: `account <name> = <config folder>`, `theme <name> = <theme>`,
  `folder <path> = <name>` (claudes in it and below must use that account, and it is used nowhere else). The default
  account (`default`) is Claude Code's own, `~\.claude` with `CLAUDE_CONFIG_DIR` unset (set to `~\.claude` it would move
  `.claude.json`). Server: `ParseAccounts`, `FolderRuleFor` (term_settings.cpp); app: parsed in `App_Frame` when
  `accounts_version` changes (the platform re-reads it on a write, once a second at most).
- Enforcement (server, scan apply): a claude whose folder's rule names another account, or of a ruled account outside
  its folders, is held from its first scan, like an account change (`tab::RuleWhy`, `TabHeld`: Esc when busy, input
  dropped, notice, tray), with the reason on its card (`msg_tab` hold-why). Which account a claude is: its config folder,
  or a ruled account's login (`ConfigLogin`, organization and email) found in it, so a work login copied into another
  folder still counts as work. A rule naming an account that doesn't exist holds every claude there. "Go on anyway"
  (`MSG_ACCOUNT_OK`) waives it for that claude process only (`tab::RuleOkPid`); `/login` never waives it.
- Starting the right one: Ctrl+Shift+A takes the account the focused folder's rule names; the palette's "Claude
  accounts" (the manager: every account with the login of an open claude of it and its folders, Enter a claude tab as
  it, the arrows step its theme into accounts.txt: `AppOutput.account_theme_name`, `SetAccountTheme`; a new name typed
  adds one) and "Add a Claude account" (the same `PaletteMode_Account`, only asking for a name; adding makes `<accounts.txt's folder>\accounts\<name>`,
  copies settings.json, CLAUDE.md, agents, commands, skills, output-styles from the main setup, never the login,
  sessions or history, appends the line, and opens a claude tab under it to /login: `AddAccount` in term_win32.c);
  sidebar sessions resume under their own account (`ScanSessions` reads every account's folder, `msg_session` config
  folder, a tag on the row). `MSG_NEW_TAB` carries the config folder. A claude typed in a shell: `clink-moss\accounts.lua`
  (shipped next to moss.exe, on `CLINK_PATH`, so existing users get it; `~\.moss` is only seeded once) sets
  `CLAUDE_CONFIG_DIR` before each prompt line from the folder's rule, and unsets one it or moss (`MOSS_ACCOUNT_DIR`) set
  on leaving.
- Seeing it: the account line starts with the account's name; every claude tab in a strip carries its account's name
  (once accounts.txt has one), tinted in that account's theme's accent, and a held one's icon is red; a pane whose
  claude's account has a theme draws in it, the default account's too (`TermPaletteFor`, `ChromeFor`; the window's
  shared chrome stays). New accounts start in the window's theme. Test: `tests/accounts` (rules held both ways, a
  leaked login, clink, a sidebar resume, Add, then a restart on a named server).

## The sessions sidebar

- Full-text search: a filter of 3 characters or more, still for 250 ms (headless runs: 2 frames, their clock moves with
  frames), goes to the server (`MSG_SEARCH_SESSIONS`), which reads every account's transcripts on its own thread in
  Windows' background mode, 4 MiB at a time, the first match in each that was said (not in an `api-request` line or a
  prompt's attachment: Claude Code 2.1 copies its system prompt and CLAUDE.md there) (`SessionsSearchThread`; a newer search stops it;
  logs "sessions search: N of M sessions, MB, ms"), and answers `MSG_SESSION_HITS` (each hit's id and the JSON string
  around the match). Hits join the filter's rows with their snippet in the accent.
- Settings `sidebar_hide` ("Sessions left out of the sidebar"; folders, `;` between): sessions in them and below aren't
  listed, unless the focused tab's folder is in one (a shell cd'd there shows that folder's).
- The sidebar's own notes (`%LOCALAPPDATA%\moss\sessions.txt`: `name <id> = <name>`, `pin <id>`, `archive <id>`; headless
  and test runs: none): a right-click on a row's menu renames (in the row: Enter keeps, empty = its first prompt), pins
  (first, under "Pinned") or archives it (left out unless something's typed; marked "archived").
- The sidebar on the window's left lists Claude Code's sessions; it starts open (layout.txt keeps `SidebarClosed`).
  Ctrl+Shift+S gives it the keyboard (again: closes it; a click in a pane takes the keyboard back). It lists Claude Code's sessions
  newest first under day headings; typing filters (project, first prompt, folder); Enter or a click brings up the
  tab running it (marked with a dot), else resumes it in a new tab of the focused pane (`OpenSession`:
  `MSG_NEW_TAB` with the session id, in its folder). The panes start after it (`Panes_ComputePixels` takes an
  origin). Open or not is kept in layout.txt (`Sidebar`). Without the keyboard it highlights the focused tab's session
  (scrolled into view; nothing for a tab that isn't a claude), with it the keyboard's cursor (`App.side_cursor`).
- The server reads the sessions (`ScanSessions`, unchanged transcripts not read again) on its own thread every 5 s,
  only while the window shows the list (`MSG_WANT_SESSIONS`), and sends them when they change (`MSG_SESSIONS`).
  Tests point `CLAUDE_CONFIG_DIR` at made-up transcripts (`tests/sessions`).

## Settings

- Ctrl+, (`settings`, or "Settings" in the palette) is the palette in its settings mode (`PaletteMode_Settings`,
  its own mode): a row per `g_Settings` entry under group headings, typing filters (label, group, note).
  Choices (theme, font, text size, opacity) step with Left/Right or the row's arrows, Enter steps forward; check
  boxes toggle on Enter or a click; texts (agent command, agent programs) edit in their row (Enter keeps, Esc drops);
  the last opens the settings folder `~\.moss` (settings.txt and the shell setup; `AppOutput.open_settings`).
  "Background image" opens a file dialog (`choose_background`); its strength steps in 5% (0 = off).
- Each change applies at once and writes its one key (`AppOutput.settings_key`); headless runs never write. The font
  list is the installed monospace families (`TermCells_MonoFonts`, DirectWrite), read the first time the settings
  open (`want_fonts`); `AppOutput.term_font` switches the cells' font. Server-read keys (`agents`, `notifications`,
  `vs_env`) take effect at their next read: the next scan, notice, or new tab.
- Keys a zeroed setting means "on" for: `hidden_files = off`, `folder_sizes = off`, `notifications = off`, `vs_env = off`,
  `dim_unfocused = off`, `claude_chat = off`. Ctrl+H in a browser writes `hidden_files` too.
- "Dim the panes not in focus" (on by default): with more than one pane showing, the others get a dark wash (a held
  claude's card stays bright above it).
- "Reset to moss's defaults" asks first (`App.reset_confirm`), then `AppOutput.reset_settings`: settings.txt becomes the
  shipped `shell\settings.txt` (the old one kept as settings-old.txt; `Term_ResetSettings`), the platform reads it again
  (`ReadSettings`, as at start) and hands it over once (`AppFrameInput.settings`, `ApplySettings`, as `App_Create`).
  The summon key and the editor stay as they were until the next start; Explorer's menu and start with Windows aren't
  settings.txt's and stay. accounts.txt isn't settings.txt's either: accounts, themes and folder rules stay.
- "Be a good programmer" (clod's, carried over) puts `~\.moss\good-programmer.md` into the global
  `<CLAUDE_CONFIG_DIR or ~\.claude>\CLAUDE.md` between moss's markers, for every Claude Code session on the machine;
  off takes the block out, the rest of the file stays (`SetGoodProgrammer`, temp file swapped in). clod's block counts
  as on, and is replaced or taken out the same way. Read at start (`AppInit.good_programmer`); headless runs only log.

## The file browser (each tab's other face)

- Barebones on purpose: list, sort, filter, select, open, copy / delete / rename / new folder / paste, drag in
  and out, folder sizes. No preview yet; features come back one by one as the user decides (`wishlist.txt`).
- Ctrl+Shift+E (`toggle_browser`) flips the focused tab between its shell and a browser of its folder
  (`PaneTab::browsing`, state in `Browser`, `term_app.c`). `term_files_win32.c` lists folders on a worker thread
  (`Files_Request`; "" is This PC, its drives with labels and free space for local ones) and watches the shown
  ones (`Files_Watch`, ReadDirectoryChangesW; a burst wakes the window a few times); results come in
  `AppFrameInput.files`, a change among a folder's own entries lists it again whole. Requests and watches go out
  after the frame's keys and clicks (they may have moved the browser). Dotfiles count as hidden.
- **Folder sizes must never hog the CPU** (an earlier version kept 8 cores busy re-measuring trees on every change).
  One sizer thread in Windows' background mode (`THREAD_MODE_BACKGROUND_BEGIN`: low CPU and I/O priority, one core
  at most), `NtQueryDirectoryFile` 64 KiB at a time, links never followed. Only local SSDs (seek-penalty IOCTL, as
  File Pilot decides) and never a drive's root. Results are cached per subfolder (`SizeRecord`); a shown folder's
  watch covers its whole tree, so a change deep down marks just that subfolder, which is measured again once it's
  been quiet 2 s, never twice in 30 s, at least once a minute while it keeps changing. Sizes measured while the
  folder was shown stay good; older ones are good for 5 minutes. A measure stops once its folder isn't shown. Each
  pass logs "sizes: ... N entries, ms, cpu ms" (C:\UnitySrc, 718k entries: 4 s on one background core, once).
  `folder_sizes = off` turns it off. The app keeps sizes by name per browser (`Browser.sizes`), so a listing
  again keeps them; sorting by size uses them.
- It opens at the shell's folder: the prompt sends OSC 9;9;<path> (`shell/clink/prompt.lua`), the emulator keeps
  it (`vt_terminal::Cwd`) and the server makes it the tab's cwd at once; the process scan only notices later.
  Later opens stay where the browser was unless the shell moved since.
- A shell is running something while its cmd has a child (not clink's background git status, recognised by
  `GIT_OPTIONAL_LOCKS=0` in its command line), from the Enter of a command line on, and no more the moment its
  prompt is back (OSC 9;9; `tab::AtPrompt`). Type-ahead can leave it idle until the next scan (1-3 s).
- Moving while the tab is idle at its prompt makes the shell follow when the browser closes (`cd "sub"` or
  `cd /d "C:\..."`, typed into the shell). Busy = `TabBusy`: a program running, an agent open (even idle), a
  claude turn. Busy means read-only: browsing anywhere, the shell stays; delete, rename, new folder and paste
  are refused with a footer notice; copy out (Ctrl+C: the file on the clipboard) and opening files stay.
- Keys: arrows/PgUp/PgDn/Home/End, Enter opens (folders here, files with their program), Backspace up (or edits
  the filter), typing filters (its characters in order: "ix" finds index.html, matches lit), Esc clears the filter, then back to the shell. F2
  renames inline, Ctrl+Shift+N new folder, Delete to the Recycle Bin, Ctrl+V pastes Explorer's copied or cut
  files (`FileOp` -> `RunFileOps` in `term_win32.c`; deletes and pastes on their own thread via
  SHFileOperation; headless runs delete for good, never show dialogs, never paste).
- Rows (and buttons, tabs, menu and palette rows) fade their hover in fast and out slower (`HoverFill`).
- Column heads sort every browser (`App.browse_sort`: 0 name, 2 modified, 4 size, +1 the other way round; folders first, names A-Z,
  dates and sizes biggest first); Ctrl+H leaves hidden files out. Shift+arrows/click select from the anchor,
  Ctrl+click toggles, Ctrl+A all, Esc lets go (`Browser.sel`: sorted name keys); copy and delete take them all.
- Drag and drop: rows dragged out go as a copy (`DragFilesOut`: SHCreateDataObject + SHDoDragDrop, modal; never
  a move). Files dragged in from other programs (`DropTarget` in `term_win32.c`, OLE; the app answers
  `AppOutput.drop_effect` for the pane under the mouse) are copied into a browser's folder (refused while its tab
  is busy) or typed into a terminal as paths, quoted where they have spaces.
- Clicked moves (a row's double click, a path part, up) run at the end of the frame: the rows' text points into the
  listing a move frees.
- The selection stays on its name across rebuilds (`cursor_name`). Listed again (a change in the folder), the row
  that was at the top stays there (`top_name`), and rows new or changed since light up and fade over 1.5 s
  (`Browser.flashes`: names diffed against the old listing).
- Cloud placeholders (OneDrive and the like: `FILE_ATTRIBUTE_OFFLINE`, `RECALL_ON_OPEN`, `RECALL_ON_DATA_ACCESS`)
  are `File_Cloud`: a cloud mark in their row, and nothing opens them unless the user does (opening downloads).
  The sizer never enters them, nor any reparse point (its listing has no reparse tags to tell links from cloud
  folders), so a OneDrive folder gets no size. Test: `tests/cloud` (offline attributes set by hand).
- Network: a mapped drive is listed like any other (sizes off: not a local SSD). A share path (`\servershare`)
  is one crumb and its own root (up from it is This PC); the shell follows to one with `pushd` (cmd can't `cd` to
  a share; pushd maps it a drive letter).
- Dates: "Today 14:03", "Oct 12", "Mar 1, 2024"; headless runs use a fixed clock (2026-06-15 12:00 UTC).

## The path bar and the go-to box

- The browser's header: Back, Forward (and the mouse's side buttons, `UiInput.side`), Up, the path's parts (a click
  goes there), the bookmark star (Ctrl+D). Each browser keeps 16 folders each way (`Browser.back` / `forward`;
  `BrowseHistory`). Alt+Left/Right stay pane focus.
- The go-to box is the palette's `PaletteMode_GoTo` (`OpenGoTo`): a click on the path bar's empty part or Ctrl+L /
  Alt+D in a browser opens it with the folder typed in; Ctrl+P / F4 in a browser, or Ctrl+Shift+G anywhere (a shell's
  tab flips to its browser), empty.
  - Typed text: `%NAME%` (the window's environment, `AppInit.environment`) and a leading `~` are filled in, `/` is
    `\`. A path lists the folder up to its last separator (`goto_request`, through the same lister) and the text after
    it ranks its entries (`FuzzyScore`, folders before files); with nothing after the separator the folder itself is
    the first row. Tab (or Right at the end) takes the selected row into the text, Alt+Up the parent, Enter goes
    (a file opens with its program), Ctrl+Enter selects it in its folder.
  - Empty: the groups Recents (newest 10, Show all), Bookmarks, Storage (the drives, how full) and Places (known
    folders, `AppInit.places`); a heading's click folds it. Other text ranks every item of every group.
  - Recents (every folder browsed, 32) and bookmarks (32) live in `%LOCALAPPDATA%\moss\paths.txt` (`recent` /
    `bookmark` lines; `AppOutput.paths_text`); headless and `--test` runs start with none and save nothing.

## Opening files: the editor, the picture view

- Enter or a double click on a file (browser, go-to box) goes through `OpenInTab`: a picture (`g_PictureTypes`) in the
  tab's picture view; a text file (`g_TextTypes`, and names with no extension) in the terminal editor, typed into the
  tab's shell when it's idle at its prompt (the browser closes, the shell follows its folder), else in a new tab beside
  it that runs it first (a shell tab's `TermNewTab.command`); anything else with its own program. The editor:
  settings.txt `editor` (`off`: the file's program), else the first of nvim, vim, hx, micro, nano, edit, emacs found on
  PATH or in Git for Windows' usr\bin (`AppFrameInput.editor`).
- The picture view (`PaneTab::viewing`, `Viewer`) covers the tab: the picture fitted, never above its own size at the
  window's DPI, a caption (name, n / count, size, keys). Arrows, Space / Backspace, PgUp / PgDn, Home / End and the
  wheel step through its folder's pictures (listed by the lister, Explorer's order, round); Esc, Enter or q go back to
  where it came from. The platform decodes on a worker thread (`ImageWorker`, `Image_Decode`, scaled to fit 4K) what
  `AppOutput.image_wants` asks (the shown picture, then its neighbours) and keeps 8 textures; headless runs decode at
  once. The tab's cells aren't watched meanwhile.
- `view [picture or folder]` in a shell (clink alias to `moss.exe view`): writes OSC 7777;view;<full path> to its
  console; OpenConsole passes it through, the server forwards it with the tab id (`MSG_OUTPUT`), the client hands it to
  the app (`AppFrameInput.view_tab`). An existing `~\.moss\clink\moss.lua` keeps its old alias until reseeded.

## Web tabs

- Ctrl+Shift+B (`new_web_tab`) opens a web tab beside the focused one, its address being typed. A web tab is a page with
  no server tab (`PaneTab::web`, `term_id` 0: the reconcile leaves it alone); WebView2 (Edge's engine, part of
  Windows 11; `vendor/webview2`, BSD: only the header and WebView2Loader.dll next to moss.exe) starts with the first
  page (`term_web.cpp`). Its address is kept in layout.txt (the tab line's last field; a blank one as about:blank), so
  it comes back after a restart. Duplicate (Ctrl+Shift+D) opens the same address; the files face doesn't apply.
- Each page draws into a DirectComposition visual under the swap chain's (`Gpu::Root`); the app draws nothing over the
  page's rect and clears to see-through while one is shown, so moss's popups go over it. The app lists every web tab in
  `AppOutput.webs` (rect, shown, mouse over it, navigate / back / forward / reload, the popup it's for) and `web_focus`;
  the platform makes, places and closes pages (`Web_Update`) and hands their url, title, history, loading,
  notifications and close requests back (`AppFrameInput.webs`). The tab's name is the page's title. A page is shown at
  once but hidden or closed only after a present (`Web_Presented`): the frame on screen until then has a see-through hole
  where the page was (switching to a shell tab flashed the windows behind).
- Mouse: the window passes its mouse messages to the page the app says is under the mouse (`Web_Mouse`,
  SendMouseInput; the wheel and side buttons go only to the page), the page's cursor shows. A press goes to the page only
  while the window is in front: a page that takes the keyboard brings its window forward (a test window's posted clicks
  did). A press that began on moss's own (a tab picked up, a splitter) stays moss's until let go: the page never sees
  the held button, the keyboard doesn't move to a page meanwhile, a page's keyboard counts as the window's (else the
  platform lets go of held buttons), and the keyboard going to a page keeps the window's mouse capture.
  Keyboard: the page has it (MoveFocus, only while the window is in front); moss's keys come back from it
  (AcceleratorKeyPressed: Ctrl+Shift+any, Alt+arrows, Ctrl+Tab, Ctrl+, Ctrl+L, F11), posted to the window as key
  messages. Ctrl+L / F6 edit the address (Enter goes: https:// added, else a Google search), F5 reloads.
- New windows (window.open, target=_blank: sign-in popups) are held (NewWindowRequested, deferred) until the app makes a
  web tab beside the opener (`AppFrameInput.web_popups`, `AppWeb.popup`); that page becomes the opener's new window
  (`put_NewWindow`), so `window.opener` works. A page that closes itself (window.close) closes its tab.
- Notifications: allowed for every page (the permission), shown by moss, not WebView2: one while its tab isn't in front
  (another tab, the window behind) goes to the tray (`AppOutput.notify_title/body` -> `MSG_NOTIFY` -> the server's
  `TrayNotify`; settings `notifications = off`: none) and puts a dot on the tab until it's shown. Other permissions
  (camera, microphone, ...) are WebView2's own prompt for now.
- Zoom: Ctrl+= / Ctrl+- / Ctrl+0 (the text size commands, on a web tab) and Ctrl+wheel over the page step its zoom
  (`PaneTab.web_zoom`, Chrome's steps `g_WebZooms`; `AppWeb.zoom` -> put_ZoomFactor); WebView2's own zoom is off. Not
  kept in the layout. A chip in the address row shows it when it isn't 100% (a click puts it back).
- Torn off (a drag out of the window, "Move tab to a new window") a web tab's page loads again in the other window: a
  page can't move between processes (every moss window is one), so a video starts over. A new window gets it with
  `--web URL` (`AppFrameInput.open_web`); the tab here goes once it's there (`AppFrameInput.web_torn_off`). A window with
  only web tabs owns nothing at the server, so tray Quit doesn't bring it back.
- History: every page a web tab goes to (`WebVisit`, `App.web_hist`: url, title, visits, last visit; 300, newest first)
  is kept in `%LOCALAPPDATA%\moss\web-history.txt` (`<last visit, s since 1601> <visits> <url>\t<title>`; headless and
  test runs: none; written at once for a visit, a title change within 10 s). While the address is edited, up to 8 of
  them show under it (`web_sugg`, drawn after the panes): those with the typed text in the address or title, ranked by
  visits over age, 8x for an address starting with it; nothing typed yet (the page's own address) shows the most
  visited. Up / Down pick one, Enter or a click goes there, Shift+Delete takes it out of the history.
- Data (logins, cookies) in `%LOCALAPPDATA%\moss\web`; test and instance runs in `%TEMP%\moss-web-<instance>`.
- Headless runs have no pages (the page's rect stays black; `tests/session` keeps a web tab across a restart).
  `moss --web-probe URL OUT.png` loads one in a window no one sees and logs what the engine can do there (Widevine,
  PlayReady, MSE, notifications, service workers, its user agent) plus a picture. A test window runs
  MOSS_TEST_COMMAND's commands (`;` between), one per posted `WM_APP + 3` (`tools/window_test.ps1 -Command`). Test
  pages that open windows or notify should do it on load: a posted click is ignored while the window is behind.

## Focus (Ctrl+Shift+F)

- The focused pane alone over the whole window: no tab strips (`caption_h` 0), no sidebar, no caption buttons
  (`App.focus_mode`; its pane is `zoom_node` meanwhile, the old zoom comes back after). On and off at once: the panes
  snap (`snap_until_frame`), no easing. Only the pane's own commands work (copy, paste, text size, scrolling, find, the
  files / shell flip, go-to, ...); other bound keys reach the program. A hint fades out in the corner. Its tab going
  ends it.

## Debugging and testing — read before changing anything visible

- **Headless runs are slow: run them only after a big batch of work, one pass, then rerun just what failed. Never
  per feature.** They must never take the user's keyboard focus: nothing outside the run (files opened, Explorer,
  clipboard, drags) — the platform logs these instead; `tools/focus_watch.ps1` checks.
- **Never pop up windows to test GUI changes.** Use headless runs (the `run-headless` skill):
  `build\moss.exe --headless --script FILE` plays a script (keys, typing, mouse by label, `waitfor` terminal
  text, `capture`, `compare`, `dump layout|screen|tabs`) against its own private server, no window. Look at
  the captured PNGs. The window path itself: `tools/window_test.ps1` (an off-screen window that never takes
  the foreground, posted keys, `-Clicks "x,y"` posted clicks at window pixels, PrintWindow capture; a test
  window takes the mouse from the posted messages, not the real cursor).
- Test sets: `tests/run_all.sh` (smoke, layout, terminal, browser, settings, find, tabs, links, shells, agents, sessions, session, windows, accounts, claude_ui); each runs in a temp folder outside
  any repo (prompts must not show git state) and compares with its local `reference/` (`bless` to accept).
  `tests/claude` is a manual set with real Claude Code. Captures named `*_varies.png` (counters, timings) are
  looked at, never compared. Headless runs ignore the user's settings.txt (defaults only).
- Debug overlay: Ctrl+Alt+Shift+F12 (frame work, GPU passes, frames drawn/skipped, IPC, glyph atlas).
- Logs: every process writes `build\logs\<time>_<mode>_<pid>.log` (`read-logs` skill); log per event, never
  per frame. Crashes and asserts write the stack, a minidump and symbol copies (`debug-crash` skill, cdb).
  Timed blocks, GPU timestamps, Superluminal markers and `MOSS_LATENCY=1` hops (`profile-frames` skill).
- Latency bar: ~1 ms key to present (0.66 ms median in a window, 0.71 ms with every core busy,
  `tools/window_test.ps1 -LatencyKeys`). The p95 (~30 ms) is clink's: about 1 key in 5 its echo waits two
  15.6 ms timer ticks, after moss already passed the key on.
  Nothing slow on the window's frame or the server's loop: process scans and the like run on their own threads.

## Rules carried from clod (don't regress)

- Quit (tray) stops the server but keeps `tabs.txt`; tabs come back on the next start (claude with --resume,
  shells in their folder with their last output). Never expose a way to end every tab and forget them.
- A shell running a program named in settings `restore_programs` (default nvim vim hx micro nano emacs less lazygit
  htop btop) runs it again after a restart: the scan keeps the cmd's child's command line (`tab::Program`, tabs.txt's
  last field; never a line with `& | < > ^ %`), cleared when the prompt is back. Quit doesn't ask about such a tab when
  nothing is lost: less, htop, btop, or an nvim whose swap files (`nvim-data\swap`: the writer's pid at byte 24, 0x55
  at byte 1007 while modified) show nothing unsaved. Test by hand: a named `--instance` run, `quitserver`, a second run.
- Never resume a claude session while an older claude of it is still alive (the server waits for them).
- Strip a parent Claude Code session's env markers from tabs (`ClaudeMarkers`).
- Real claude in tests: always end it with `/exit` (killing it during startup turns its fullscreen renderer off
  machine-wide), and delete the project folder the test made in `~/.claude/projects`.
- Keep the default `shell/init.bat` near empty; shell setup lives in clink Lua.
- Character widths must match conhost (`term_widths.h`, `CharWidth`), or rows drift.
- Edit C/C++ string escapes (`\x1b`, `\\`) with the Edit tool or a Python script file; shell heredocs and inline
  python through Bash mangle backslashes.

## Git

Commit straight to `main` when asked to commit: no feature branches (this overrides any default to branch first).

## Building

`build.bat` (any shell; finds Visual Studio): `build` (optimized, internal checks), `build debug`, `build
release`. Copies clink (GPL-3.0, a separate program run in each tab's cmd), clink-completions (MIT), ConPTY (MIT), the
default shell setup next to `moss.exe`, and the reader's fonts (`vendor\fonts`: Libre Caslon, SIL OFL) to `fonts\`. The
terminal font is `font` in settings.txt, else Cascadia Mono (Windows 11), else Consolas. Its icon (`tools/make_icon.cpp`) is clod's (a rounded
block, a serif lambda) in Moss's colors: green-black block, lime lambda in STIX Two Text SemiBold (`vendor/stix`, OFL,
loaded only by make_icon, never shipped). Links with MSVC's link.exe by full path (Git's
`link` can shadow it). From Git Bash: `cmd //c "C:\\UnitySrc\\moss\\build.bat"`. `build.bat` must stay CRLF.

## The move from term (its working name)

- moss was called term until 2026-10-28. `MigrateFromTerm` (`term_settings.cpp`, run first by `SeedUserSetup`)
  moves `~\.term` to `~\.moss` and `%LOCALAPPDATA%\term` to `%LOCALAPPDATA%\moss` once, renames the default
  `clink\clod.lua` to `moss.lua` with moss's `view`, and moves "start with Windows" and "Open in term" to moss's
  names. While a term server still runs (its pipe `term-<sid>` is there) it copies instead and leaves
  `~\.moss\.from-term`, so each start copies again until term is gone. Source files keep their `term_` names;
  the session header stays "term-layout 1".

## Releasing (GitHub, winget)

- The `release` skill (`.claude/skills/release`) walks through all of it, in order.

- The version lives in `code/moss_version.h` (the exe's version resource reads it too).
- `powershell -File tools/release/package.ps1 [-Owner kevinmkchin] [-Repo moss]`: a release build, then
  `dist\moss-<version>-win-x64.zip` (moss.exe; ConPTY, clink, clink-completions and the default shell straight from the
  repo, not `build\`, which keeps files the repo dropped; LICENSE as LICENSE.txt, THIRD-PARTY.txt; no README) and its winget
  manifests in `dist\winget\manifests\n\NoclipSoftware\Moss\<version>` (`tools/release/winget/*.yaml`, the zip's
  SHA256 and its release URL filled in; `winget validate --manifest` them). The dev build is gone after it: run
  `build.bat` again.
- winget: a zip with a nested portable `moss.exe`, `ArchiveBinariesDependOnPath` (its folder goes on PATH, so moss
  runs from its own folder); `ExeDir` also follows links in case it's ever started through one.
- Order: tag `v<version>`, a GitHub release with the zip, then a pull request to microsoft/winget-pkgs with the three
  manifests (`wingetcreate submit` does it). Publishing anything is the user's call, every time.

## Licensing

moss is GPL-3.0 (`LICENSE`); it ships clink (GPL-3.0), clink-completions (MIT), ConPTY (MIT), WebView2's loader (BSD) and
the Libre Caslon fonts (SIL OFL) next to moss.exe
(`tools/release/THIRD-PARTY.txt`). Every line of moss is its own or the user's (clod's code is the user's). File
Pilot is a reference for look and feel only (screenshots, the user's descriptions): none of its code, assets (icon
font, scheme names) or internals come into the repository; its features come back one by one, written from
scratch, as the user decides.
