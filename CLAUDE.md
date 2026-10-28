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
## The sessions sidebar

- The sidebar on the window's left lists Claude Code's sessions; it starts open (layout.txt keeps `SidebarClosed`).
  Ctrl+Shift+S gives it the keyboard (again: closes it; a click in a pane takes the keyboard back). It lists Claude Code's sessions
  newest first under day headings; typing filters (project, first prompt, folder); Enter or a click brings up the
  tab running it (marked with a dot), else resumes it in a new tab of the focused pane (`OpenSession`:
  `MSG_NEW_TAB` with the session id, in its folder). The panes start after it (`Panes_ComputePixels` takes an
  origin). Open or not is kept in layout.txt (`Sidebar`).
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
- Keys a zeroed setting means "on" for: `hidden_files = off`, `folder_sizes = off`, `notifications = off`, `vs_env = off`. Ctrl+H in a
  browser writes `hidden_files` too.

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
- Test sets: `tests/run_all.sh` (smoke, layout, terminal, browser, settings, find, tabs, links, shells, agents, sessions, session, windows); each runs in a temp folder outside
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
- Never resume a claude session while an older claude of it is still alive (the server waits for them).
- Strip a parent Claude Code session's env markers from tabs (`ClaudeMarkers`).
- Real claude in tests: always end it with `/exit` (killing it during startup turns its fullscreen renderer off
  machine-wide), and delete the project folder the test made in `~/.claude/projects`.
- Keep the default `shell/init.bat` near empty; shell setup lives in clink Lua.
- Character widths must match conhost (`term_widths.h`, `CharWidth`), or rows drift.
- Edit C/C++ string escapes (`\x1b`, `\\`) with the Edit tool or a Python script file; shell heredocs and inline
  python through Bash mangle backslashes.

## Building

`build.bat` (any shell; finds Visual Studio): `build` (optimized, internal checks), `build debug`, `build
release`. Copies clink (GPL-3.0, a separate program run in each tab's cmd), clink-completions (MIT), ConPTY (MIT), the
default shell setup next to `moss.exe`. moss ships no fonts: the terminal font is `font` in settings.txt,
else Cascadia Mono (Windows 11), else Consolas. Its icon (`tools/make_icon.cpp`) is clod's (a rounded
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

moss is GPL-3.0 (`LICENSE`); it ships clink (GPL-3.0), clink-completions (MIT) and ConPTY (MIT) next to moss.exe
(`tools/release/THIRD-PARTY.txt`). Every line of moss is its own or the user's (clod's code is the user's). File
Pilot is a reference for look and feel only (screenshots, the user's descriptions): none of its code, assets (icon
font, scheme names) or internals come into the repository; its features come back one by one, written from
scratch, as the user decides.
