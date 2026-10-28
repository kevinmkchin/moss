// The window <-> server protocol. The server owns the tabs (ConPTY, emulator, scrollback); the window
// owns the layout and draws. The server streams each tab the window shows as rows of cells, and the
// tab list whenever it changes; the window sends input, sizes and tab requests, all by tab id.
// Frame = u32 Type, u32 Size, Size bytes of payload (term_ipc.cpp).

enum msg_type
{
    MSG_NONE = 0,
    MSG_ATTACH,    // window -> server: msg_attach, then the window's cwd (UTF-8), a 0 byte, the window's name (empty = the main window)
    MSG_BYE,       // server -> window: reason text, then a 0 byte and a BYE_* byte (older windows read just the text)
    MSG_TABS,      // server -> window: msg_tabs, then per tab msg_tab and its strings (key, name, cwd, notice, agent, owner)
    MSG_CELLS,     // server -> window: msg_cells, then RowCount x (s32 Y, cell[Cols]); one tab per message
    MSG_OUTPUT,    // server -> window: what programs pass to the terminal itself (OSC 52 clipboard) and BEL
    MSG_WATCH,     // window -> server: u32 Count, then Count x msg_watch: every tab the window has, its size, shown or not
    MSG_INPUT,     // window -> server: u32 TabId, then VT input bytes for its program
    MSG_NEW_TAB,   // window -> server: msg_new_tab, then the cwd (UTF-8), a 0 byte, the claude session to resume (or empty), a 0 byte, the
                   // agent command (NEW_TAB_AGENT)
    MSG_CLOSE_TAB, // window -> server: u32 TabId
    MSG_SCROLL,    // window -> server: u32 TabId, s32 Lines (> 0 back into the scrollback; SCROLL_TO_BOTTOM = live)
    MSG_FOCUS,     // window -> server: u32 TabId with the keyboard focus (0 = the window lost it): focus reports
    MSG_DEBUG,     // window -> server: u32 DEBUG_* (tests: crash the server, quit it)
    MSG_SESSIONS,  // server -> window: u32 Count, then Count x (msg_session, its strings): Claude Code's sessions,
                   // newest first, while the window wants them; again whenever they change
    MSG_WANT_SESSIONS, // window -> server: u32 On: the sessions list is on screen (read every 5 s while it is)
    MSG_SET_OWNER, // window -> server: u32 TabId, then the name of the window it now belongs to (empty = the main one)
    MSG_FIND,      // window -> server: u32 TabId, u32 FIND_*, then the text to find (UTF-8; empty = find is off): matches are
                   // marked in the tab's cells (CELL_FOUND), FIND_OLDER/NEWER step the current one (scrolled into view)
    MSG_FIND_STATE, // server -> window: u32 TabId, u32 Count (matches in the scrollback and screen), u32 Index (1-based; 0 = none),
                    // then u16s: where matches are, as a share of all the lines (0..65535; one per place, at most 512)
    MSG_LINK_GET,  // window -> server: u32 TabId, u32 LinkId (a cell's, Ctrl+clicked): the server answers with MSG_LINK
    MSG_LINK,      // server -> window: u32 TabId, then the link's URI (to open)
    MSG_HISTORY_GET, // window -> server: u32 TabId: its claude session's history (the transcript), answered with MSG_HISTORY
    MSG_HISTORY,   // server -> window: u32 TabId, u32 Count, then Count x (u8 FromUser, u32 TimeLen, u32 TextLen, time, text),
                   // oldest first (no session: Count 0)
};

enum
{
    BYE_RECONNECT = 0, // the window says why and tries again (another build's server, ...)
    BYE_CLOSE     = 1, // the window closes: term quit from the tray, or another window took over
};

enum
{
    FIND_CASE  = 1 << 0, // match case
    FIND_OLDER = 1 << 1, // the current match moves to the one before it (up)
    FIND_NEWER = 1 << 2, // ... after it (down)
};

enum
{
    DEBUG_CRASH = 1, // a failed assert in the server (its crash path)
    DEBUG_QUIT  = 2, // quit like the tray's Quit (tabs.txt kept)
};

// Bump whenever a message's layout or meaning changes: a window from another build is turned away
// with a clear message instead of misreading the stream.
#define TERM_PROTOCOL_VERSION 12

struct msg_attach
{
    u32 Version;   // TERM_PROTOCOL_VERSION; first, so a window from another build fails the check
    u32 Flags;     // none yet
    u64 Window;    // the window's HWND: the tray brings it forward, notifications check its foreground
};

enum
{
    TAB_AGENT       = 1 << 0, // a CLI agent runs in it (msg_tab's agent string names it; claude: Key is its session id)
    TAB_BUSY        = 1 << 1, // claude's title spinner runs: a turn is under way
    TAB_NEEDS_INPUT = 1 << 2, // claude rang or notified (a permission prompt, a question) and nothing was typed since
    TAB_EXITED      = 1 << 3, // its cmd ended or never started
    TAB_RUNNING     = 1 << 4, // a shell running a program (its cmd has a child process)
};

struct msg_tabs
{
    u32 Count;
};

struct msg_tab
{
    u32 Id;        // the server's id for the tab, never reused while the server runs
    u32 Request;   // the msg_new_tab.Request that opened it (0 = restored or opened otherwise)
    u32 Flags;     // TAB_*
    float Cpu;     // percent of the whole machine its cmd and everything under it used lately
    u32 KeyLen;    // lasting id: the claude session id, else the shell's history id (survives restarts)
    u32 NameLen;   // what to call it ("❯ C:\src", "vim", claude's title)
    u32 CwdLen;
    u32 NoticeLen; // why it wants a look ("finished", "needs you", "bell"), until the window shows it
    u32 AgentLen;  // the agent running in it ("claude", "codex", "gemini", ...); empty = a plain shell
    u32 OwnerLen;  // the window it belongs to (empty = the main window)
};

struct msg_watch
{
    u32 TabId;
    s32 Cols;      // its pane's size in cells: the server sizes the tab (and its ConPTY) to it
    s32 Rows;
    u32 Shown;     // a pane shows it: its cells are streamed
};

enum
{
    NEW_TAB_SHELL  = 0,
    NEW_TAB_CLAUDE = 1, // a new claude session, or the session named after the cwd (resume)
    NEW_TAB_AGENT  = 2, // the shell runs the agent command named after the session (codex, gemini, ...)
};

struct msg_new_tab
{
    u32 Request;   // the window's own number for it, echoed in msg_tab.Request
    u32 Kind;      // NEW_TAB_*
    s32 Cols;
    s32 Rows;
};

#define SCROLL_TO_BOTTOM (-0x7fffffff)
#define SCROLL_PREV_MARK (-0x7ffffffe)   // the prompt above the view's top line at the top
#define SCROLL_NEXT_MARK (-0x7ffffffd)   // the next prompt below it at the top (live once there is none)
#define SCROLL_CLEAR     (-0x7ffffffc)   // forget the scrollback (the screen stays)
#define SCROLL_COPY_ALL  (-0x7ffffffb)   // the scrollback and screen as text, to the window's clipboard (OSC 52 in MSG_OUTPUT)

enum
{
    CELLS_FULL            = 1 << 0, // every row follows: the window's grid for the tab is resized to Cols x Rows
    CELLS_CURSOR_HIDDEN   = 1 << 1,
    CELLS_ALT_SCREEN      = 1 << 2,
    CELLS_APP_CURSOR_KEYS = 1 << 3, // DECCKM: arrows send ESC O A
    CELLS_BRACKETED_PASTE = 1 << 4,
    CELLS_FOCUS_EVENTS    = 1 << 5,
    CELLS_SYNC            = 1 << 6, // mid synchronized update (DEC 2026): the server held it as long as it could
    CELLS_MOUSE_SGR       = 1 << 7, // mouse reports in SGR form (1006), else the X10 bytes
    CELLS_WIN32_INPUT     = 1 << 8, // ConPTY asked for win32-input-mode (9001): keys as ESC [ Vk;Sc;Uc;Kd;Cs;Rc _
};

struct msg_session
{
    u64 Modified;     // its transcript's last write (FILETIME)
    u32 IdLen;        // then the id, the folder it ran in, that folder's name, its first prompt (UTF-8)
    u32 CwdLen;
    u32 ProjectLen;
    u32 TitleLen;
};

struct msg_cells
{
    u32 TabId;
    s32 Cols;
    s32 Rows;
    s32 CursorX;
    s32 CursorY;
    s32 CursorShape;  // DECSCUSR value (0 default, 1/2 block, 3/4 underline, 5/6 bar; odd blinks)
    u32 Flags;        // CELLS_*
    s32 MouseMode;    // the program's mouse tracking: 0 none, 1000 clicks, 1002 drags, 1003 all motion
    s32 ScrollOffset; // lines scrolled back from the live screen (0 = live)
    s32 Scrollback;   // lines of scrollback there are
    u32 RowCount;
};
