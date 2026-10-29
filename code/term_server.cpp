// Server: the background process that owns every tab (ConPTY children, emulators, scrollback) and
// shows the tray icon. The window attaches over the pipe; the server streams the cells of the tabs it
// shows and the tab list, and runs the window's input, sizes and tab requests (term_ipc_proto.h).
// It outlives the window; it ends on an explicit quit, or when no tabs remain and nobody is attached.
// Several windows can attach at once (one process each); every tab belongs to one window (tab::Owner).

struct client;

struct tab
{
    u32 Id;                 // the window's handle on it (never reused while this server runs)
    u32 Request;            // the window's msg_new_tab.Request that opened it; 0 = restored
    vt_terminal Term;
    // Every tab runs cmd. It is a claude tab while claude runs inside that cmd: SessionId is then
    // that claude's session (empty = plain shell).
    u32 ClaudePid;          // the claude found running in this tab's cmd; 0 = none seen (yet)
    u64 LaunchedAtMs;       // the server started claude here and hasn't seen it yet; 0 = not waiting
    std::string SessionId;
    std::string Cwd;
    std::string Project;
    std::string Title;      // from the session file; used when the child sets no title
    std::string Agent;      // the CLI agent running in it: "claude" (SessionId set) or another found by the scan; empty = shell
    std::wstring AgentCommand; // started with the tab after init (NEW_TAB_AGENT), once
    s32 ScrollOffset;       // lines scrolled back from live output; 0 = following
    u64 SeenPushed;         // Term.ScrollbackPushed as of the last scroll fix-up
    std::string Notice;     // why it wants a look ("finished", "needs you", ...), set while the window didn't show it
    bool32 Exited;          // cmd ended, or never started
    bool32 TitleBusy;       // claude's title shows its spinner (a turn is running)
    bool32 NeedsInput;      // claude asked for the user (bell, OSC 9) and nothing has been typed into it since
    bool32 Running;         // a shell running a program: its cmd has a child (not the prompt's own git status); busy from
                            // the Enter of a command line, idle the moment its prompt is back (OSC 9;9)
    std::string Program;    // the command line of the program it runs, when a restart runs it again (settings
                            // `restore_programs`); kept in tabs.txt
    bool32 PromptSeen;      // its prompt reports itself (OSC 9;9)
    bool32 AtPrompt;        // the prompt printed and no Enter went in since
    u64 EnterAtMs;          // the last Enter typed into it
    float Cpu;              // percent of the whole machine its cmd and everything under it used lately
    u64 NotifiedAtMs;       // last tray notification about this tab
    std::string HistoryId;  // names the file a shell tab's recent output is saved to (HistoryText)
    std::string HistorySaved;
    bool32 ReplayHistory;   // restored after a restart: cmd types the saved output before its prompt

    // --- what the window has of it ---
    std::string Owner;      // the window it belongs to (its name; empty = the main window), kept in tabs.txt
    client* Viewer;         // the window a pane of which shows it (msg_watch): its cells are streamed there; 0 = none
    std::vector<u32> FindText; // what the window's find bar looks for (folded when not FindCase); empty = off
    bool32 FindCase;
    bool32 FindHas;         // a current match: its absolute line (Term.ScrollbackPushed counts) and first column
    s64 FindLine;
    s32 FindCol;
    bool32 SentValid;       // Sent holds what the window has; else the next frame is full
    std::vector<cell> Sent; // the rows last sent
    msg_cells SentHead;
    u64 SyncSinceMs;        // when it began a synchronized update (frames hold up to 100 ms for its end)
    pty* Pty;
};

struct server_state
{
    std::vector<tab*> Tabs;
    std::string LaunchCwd;            // where tabs start when their folder is gone: the window's cwd, else home
    std::vector<std::wstring> VsSet;  // Visual Studio's build environment (LoadVsEnv)
    std::vector<std::wstring> VsPrepend;
    std::vector<std::wstring> VsAppend;
    u32 NextId;
    bool32 Quit;
};

// The command that starts claude, or empty when Claude Code isn't installed. Looked up on every
// tab start, so installing it while term runs works on the next tab.
static std::wstring FindClaude()
{
    // MOSS_CLAUDE replaces the claude command, e.g. "cmd.exe /k echo" for tests.
    wchar_t Override[1024] = {};
    if(GetEnvironmentVariableW(L"MOSS_CLAUDE", Override, ArrayCount(Override))) { return Override; }
    wchar_t Path[MAX_PATH] = {};
    if(SearchPathW(0, L"claude.exe", 0, MAX_PATH, Path, 0)) { return std::wstring(L"\"") + Path + L"\""; }
    if(SearchPathW(0, L"claude.cmd", 0, MAX_PATH, Path, 0)) { return L"cmd.exe /c claude"; } // npm install
    // The native installer's location. Checked directly because this server's PATH was inherited
    // when it started and doesn't see installs made since.
    wchar_t Home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
    std::wstring Native = std::wstring(Home) + L"\\.local\\bin\\claude.exe";
    if(GetFileAttributesW(Native.c_str()) != INVALID_FILE_ATTRIBUTES) { return L"\"" + Native + L"\""; }
    return std::wstring();
}

// The environment markers a parent Claude Code session leaves behind. A claude that inherits them
// thinks it's a subagent (e.g. it stops saving its transcript), so tabs start without them.
static const wchar_t* ClaudeMarkers[] = {
    L"CLAUDECODE", L"CLAUDE_CODE_CHILD_SESSION", L"CLAUDE_CODE_SESSION_ID", L"CLAUDE_CODE_SESSION_ATTENDED",
    L"CLAUDE_CODE_ENTRYPOINT", L"CLAUDE_CODE_EXECPATH", L"CLAUDE_CODE_MESSAGING_SOCKET",
    L"CLAUDE_CODE_MESSAGING_TOKEN", L"CLAUDE_PID", L"AI_AGENT",
};

static std::string NewUuid()
{
    u32 R[4] = {};
    for(u32& V : R) { rand_s(&V); }
    R[1] = (R[1] & 0xFFFF0FFF) | 0x00004000; // version 4
    R[2] = (R[2] & 0x3FFFFFFF) | 0x80000000; // RFC 4122 variant
    char Buf[40];
    snprintf(Buf, sizeof(Buf), "%08x-%04x-%04x-%04x-%04x%08x", R[0], R[1] >> 16, R[1] & 0xFFFF, R[2] >> 16, R[2] & 0xFFFF, R[3]);
    return Buf;
}

static s32 TextWidth(const std::string& S)
{
    s32 W = 0;
    size_t I = 0;
    while(I < S.size()) { W += CharWidth(DecodeUtf8(S.data(), S.size(), &I)); }
    return W;
}

static std::string TruncateText(const std::string& S, s32 MaxW)
{
    if(MaxW <= 0) { return std::string(); }
    if(TextWidth(S) <= MaxW) { return S; }
    std::string Out;
    s32 W = 0;
    size_t I = 0;
    while(I < S.size())
    {
        size_t Start = I;
        s32 CW = CharWidth(DecodeUtf8(S.data(), S.size(), &I));
        if(W + CW > MaxW - 1) { break; }
        Out.append(S, Start, I - Start);
        W += CW;
    }
    return Out + "\xE2\x80\xA6"; // …
}

static std::string ToLower(const std::string& S)
{
    std::string Out = S;
    for(char& C : Out) { if(C >= 'A' && C <= 'Z') { C = (char)(C - 'A' + 'a'); } }
    return Out;
}

static std::string ProjectName(const std::string& Cwd)
{
    size_t Slash = Cwd.find_last_of("\\/");
    return (Slash == std::string::npos) ? Cwd : Cwd.substr(Slash + 1);
}

// The session a running claude registered in <ClaudeConfigDir>\sessions\<pid>.json. False when
// there's no such file, it names no session, or it's left over from an earlier process that had
// the same pid (its recorded start time differs).
static bool32 ReadClaudeSession(DWORD Pid, std::string& Id, std::string& Cwd)
{
    std::wstring Path = ClaudeConfigDir() + L"\\sessions\\" + std::to_wstring(Pid) + L".json";
    HANDLE File = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { return false; }
    std::string Json = ReadFileRange(File, 0, 64 * 1024);
    CloseHandle(File);
    std::string Start;
    HANDLE Proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);
    FILETIME Created = {}, Ignored[3] = {};
    bool32 Timed = Proc && GetProcessTimes(Proc, &Created, &Ignored[0], &Ignored[1], &Ignored[2]);
    if(Proc) { CloseHandle(Proc); }
    u64 CreatedFt = ((u64)Created.dwHighDateTime << 32) | Created.dwLowDateTime;
    if(JsonFindString(Json, "procStart", false, Start) && Timed && Start != std::to_string(CreatedFt)) { return false; }
    if(!JsonFindString(Json, "sessionId", false, Id) || Id.empty()) { return false; }
    Cwd.clear();
    JsonFindString(Json, "cwd", false, Cwd);
    return true;
}

// A process's current directory (where a shell has cd'd to), read from its process parameters
// (PEB -> RTL_USER_PROCESS_PARAMETERS.CurrentDirectory, 64-bit layout). Empty if it can't be read.
static std::string ProcessCurrentDirectory(DWORD Pid)
{
    typedef LONG (WINAPI *query_fn)(HANDLE, PROCESSINFOCLASS, void*, ULONG, ULONG*);
    static query_fn Query = (query_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    HANDLE Proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, Pid);
    if(!Proc || !Query) { if(Proc) { CloseHandle(Proc); } return std::string(); }
    PROCESS_BASIC_INFORMATION Basic = {};
    u64 Params = 0;
    UNICODE_STRING Dir = {};
    wchar_t Buf[MAX_PATH + 1] = {};
    bool32 Ok = Query(Proc, ProcessBasicInformation, &Basic, sizeof(Basic), 0) >= 0 &&
                ReadProcessMemory(Proc, (u8*)Basic.PebBaseAddress + 0x20, &Params, sizeof(Params), 0) &&
                ReadProcessMemory(Proc, (u8*)(uintptr_t)Params + 0x38, &Dir, sizeof(Dir), 0) &&
                Dir.Length > 0 && Dir.Length <= MAX_PATH * sizeof(wchar_t) &&
                ReadProcessMemory(Proc, Dir.Buffer, Buf, Dir.Length, 0);
    CloseHandle(Proc);
    if(!Ok) { return std::string(); }
    std::wstring Path(Buf, Dir.Length / sizeof(wchar_t));
    if(Path.size() > 3 && Path.back() == L'\\') { Path.pop_back(); } // "C:\foo\" -> "C:\foo", "C:\" stays
    return WideToUtf8(Path);
}

// A process's command line, read from its process parameters like ProcessCurrentDirectory (64-bit
// layout, RTL_USER_PROCESS_PARAMETERS.CommandLine). Empty if it can't be read.
static std::wstring ProcessCommandLine(DWORD Pid)
{
    typedef LONG (WINAPI *query_fn)(HANDLE, PROCESSINFOCLASS, void*, ULONG, ULONG*);
    static query_fn Query = (query_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    HANDLE Proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, Pid);
    if(!Proc || !Query) { if(Proc) { CloseHandle(Proc); } return std::wstring(); }
    PROCESS_BASIC_INFORMATION Basic = {};
    u64 Params = 0;
    UNICODE_STRING Line = {};
    std::wstring Out;
    if(Query(Proc, ProcessBasicInformation, &Basic, sizeof(Basic), 0) >= 0 &&
       ReadProcessMemory(Proc, (u8*)Basic.PebBaseAddress + 0x20, &Params, sizeof(Params), 0) &&
       ReadProcessMemory(Proc, (u8*)(uintptr_t)Params + 0x70, &Line, sizeof(Line), 0) && Line.Length > 0 && Line.Length <= 8192)
    {
        Out.resize(Line.Length / sizeof(wchar_t));
        if(!ReadProcessMemory(Proc, Line.Buffer, &Out[0], Line.Length, 0)) { Out.clear(); }
    }
    CloseHandle(Proc);
    return Out;
}

// A settings.txt list of program names (separated by spaces or commas), lowercase; Default when unset.
// `agents`: the CLI agents the scan looks for: a process under a tab's cmd whose exe has the name, or a
// script host (node, bun, deno, python) running a script whose path has a folder or file of that name
// (npm installs: gemini, codex, ...). `restore_programs`: what a shell tab runs again after a restart.
static std::vector<std::string> SettingNames(const char* Key, const char* Default)
{
    std::string Setting = ReadSetting(Key);
    if(Setting.empty()) { Setting = Default; }
    std::vector<std::string> Out;
    for(size_t At = 0; At < Setting.size();)
    {
        size_t End = Setting.find_first_of(" ,", At);
        if(End == std::string::npos) { End = Setting.size(); }
        if(End > At) { Out.push_back(ToLower(Setting.substr(At, End - At))); }
        At = End + 1;
    }
    return Out;
}

// One tab's answer from a process scan: the claude running somewhere under its cmd, if any, the
// folder the cmd itself is in, whether it runs anything, and the CPU its tree used.
struct claude_find
{
    DWORD CmdPid;
    DWORD Pid;       // 0 = none
    std::string Id;
    std::string Cwd;
    std::string ShellCwd;
    std::string Agent;   // another CLI agent running under it (settings `agents`), when there's no claude
    bool32 HasChild; // the cmd runs a program
    std::string Program; // its command line, when it's one to run again after a restart (settings `restore_programs`)
    u64 CpuUsed;     // CPU time the cmd and everything under it used since the last scan (100 ns units)
};

// Claude Code's sessions, re-read on their own thread (hundreds of transcripts): Known in (unchanged transcripts
// aren't read again), Sessions out, then Done and the loop's Wake.
struct sessions_scan
{
    std::vector<session_info> Known;
    std::vector<session_info> Sessions;
    HANDLE Wake;
    volatile LONG Done;
};

static DWORD WINAPI SessionsScanThread(void* Param)
{
    Debug_NameThread("sessions scan");
    sessions_scan* S = (sessions_scan*)Param;
    S->Sessions = ScanSessions(S->Known);
    InterlockedExchange(&S->Done, 1);
    SetEvent(S->Wake);
    return 0;
}

// A claude session's history read on its own thread (transcripts can be tens of megabytes): SessionId in,
// Reply (MSG_HISTORY's payload) out, then Done and the loop's Wake.
struct history_read
{
    u32 TabId;
    std::string SessionId;
    std::string Reply;
    HANDLE Wake;
    volatile LONG Done;
};

static DWORD WINAPI HistoryReadThread(void* Param)
{
    Debug_NameThread("history read");
    history_read* H = (history_read*)Param;
    H->Reply = ReadHistory(H->TabId, H->SessionId);
    InterlockedExchange(&H->Done, 1);
    SetEvent(H->Wake);
    return 0;
}

// The sessions list for the window: msg_session and its strings each, the newest 500.
static std::string EncodeSessions(const std::vector<session_info>& Sessions)
{
    std::string Out;
    u32 Count = (u32)Min((s32)Sessions.size(), 500);
    Out.append((const char*)&Count, 4);
    for(u32 I = 0; I < Count; ++I)
    {
        const session_info& S = Sessions[I];
        msg_session M = {S.ModifiedTime, (u32)S.Id.size(), (u32)S.Cwd.size(), (u32)S.Project.size(), (u32)S.Title.size()};
        Out.append((const char*)&M, sizeof(M));
        Out += S.Id;
        Out += S.Cwd;
        Out += S.Project;
        Out += S.Title;
    }
    return Out;
}

// A process scan handed to its own thread: CmdPids in, Finds out, then Done and the loop's Wake.
struct claude_scan
{
    std::vector<DWORD> CmdPids;
    std::vector<std::string> Agents; // in: the CLI agents to look for by name (settings `agents`)
    std::vector<std::string> Programs; // in: the programs a restart runs again (settings `restore_programs`)
    std::vector<claude_find> Finds;
    std::map<std::pair<DWORD, u64>, u64> Cpu; // in: each tab process's CPU time at the last scan, by pid and start time; out: now
    u64 PrevAtFt;  // in: when the last scan read them (0 = none yet)
    u64 AtFt;      // out: when this one did
    HANDLE Wake;
    volatile LONG Done;
};

// Claude Code writes <ClaudeConfigDir>\sessions\<pid>.json for each running instance. A tab is a
// claude tab while one of those pids lives somewhere under its cmd; the file names the session,
// so a claude typed in a shell (or switched with /resume) is tracked too. Also keeps the tabs'
// console hosts (ConPTY starts them as our children) and claudes above normal priority, like us.
static DWORD WINAPI ClaudeScanThread(void* Param)
{
    Debug_NameThread("claude scan");
    claude_scan* S = (claude_scan*)Param;
    TIMED_BLOCK(ClaudeScan);
    std::vector<PROCESSENTRY32W> Procs = ListProcesses();
    FILETIME NowFt = {};
    GetSystemTimeAsFileTime(&NowFt);
    S->AtFt = ((u64)NowFt.dwHighDateTime << 32) | NowFt.dwLowDateTime;
    std::map<std::pair<DWORD, u64>, u64> Cpu;
    for(const PROCESSENTRY32W& P : Procs)
    {
        if(P.th32ParentProcessID != GetCurrentProcessId() || (_wcsicmp(P.szExeFile, L"conhost.exe") != 0 && _wcsicmp(P.szExeFile, L"OpenConsole.exe") != 0)) { continue; }   // the console hosts
        HANDLE Host = OpenProcess(PROCESS_SET_INFORMATION, FALSE, P.th32ProcessID);
        if(Host) { RaiseForTyping(Host); CloseHandle(Host); }
    }
    for(DWORD CmdPid : S->CmdPids)
    {
        std::vector<DWORD> Tree = {CmdPid};
        for(size_t K = 0; K < Tree.size() && Tree.size() < 256; ++K)
        {
            for(const PROCESSENTRY32W& P : Procs)
            {
                if(P.th32ParentProcessID == Tree[K] && P.th32ProcessID != Tree[K]) { Tree.push_back(P.th32ProcessID); }
            }
        }
        claude_find F = {};
        F.CmdPid = CmdPid;
        F.ShellCwd = ProcessCurrentDirectory(CmdPid);
        // a program runs: a child of the cmd that isn't the prompt's own git status (clink runs it in the
        // background without optional locks)
        for(const PROCESSENTRY32W& P : Procs)
        {
            if(P.th32ParentProcessID != CmdPid || P.th32ProcessID == CmdPid) { continue; }
            std::string Line = WideToUtf8(ProcessCommandLine(P.th32ProcessID));
            if(Line.find("GIT_OPTIONAL_LOCKS=0") != std::string::npos || Line.find("--no-optional-locks") != std::string::npos) { continue; }
            F.HasChild = true;
            // one to run again after a restart; never a line cmd would read as more than one command (its
            // escapes and variables are gone from what the child got)
            std::string Exe = ToLower(WideToUtf8(P.szExeFile));
            if(Exe.size() > 4 && Exe.compare(Exe.size() - 4, 4, ".exe") == 0) { Exe.resize(Exe.size() - 4); }
            if(std::find(S->Programs.begin(), S->Programs.end(), Exe) != S->Programs.end() && Line.find_first_of("&|<>^%\r\n") == std::string::npos)
            {
                F.Program = Line;
            }
        }
        // --- the CPU the tree used since the last scan: each process's time now less what it had then,
        // all of it for one started since (a process that ended in between takes its last slice along) ---
        for(DWORD Pid : Tree)
        {
            HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);
            FILETIME Created = {}, Ended = {}, Kernel = {}, User = {};
            if(Process && GetProcessTimes(Process, &Created, &Ended, &Kernel, &User))
            {
                u64 CreatedFt = ((u64)Created.dwHighDateTime << 32) | Created.dwLowDateTime;
                u64 Used = (((u64)Kernel.dwHighDateTime << 32) | Kernel.dwLowDateTime) + (((u64)User.dwHighDateTime << 32) | User.dwLowDateTime);
                std::pair<DWORD, u64> Key = {Pid, CreatedFt};
                auto Was = S->Cpu.find(Key);
                if(Was != S->Cpu.end()) { F.CpuUsed += Used > Was->second ? Used - Was->second : 0; }
                else if(S->PrevAtFt && CreatedFt >= S->PrevAtFt) { F.CpuUsed += Used; }
                Cpu[Key] = Used;
            }
            if(Process) { CloseHandle(Process); }
        }
        for(size_t K = 1; K < Tree.size() && !F.Pid; ++K)
        {
            if(ReadClaudeSession(Tree[K], F.Id, F.Cwd)) { F.Pid = Tree[K]; }
        }
        // --- no claude: another CLI agent, by its exe's name or its script's path ---
        for(size_t K = 1; K < Tree.size() && !F.Pid && F.Agent.empty(); ++K)
        {
            std::string Exe;
            for(const PROCESSENTRY32W& P : Procs) { if(P.th32ProcessID == Tree[K]) { Exe = ToLower(WideToUtf8(P.szExeFile)); } }
            if(Exe.size() > 4 && Exe.compare(Exe.size() - 4, 4, ".exe") == 0) { Exe.resize(Exe.size() - 4); }
            bool32 Host = Exe == "node" || Exe == "bun" || Exe == "deno" || Exe == "python" || Exe == "python3" || Exe == "pythonw";
            std::string Line = Host ? ToLower(WideToUtf8(ProcessCommandLine(Tree[K]))) : std::string();
            for(const std::string& A : S->Agents)
            {
                if(!F.Agent.empty()) { break; }
                if(Exe == A) { F.Agent = A; }
                for(const char* Sep : {"\\", "/"})
                {
                    for(size_t At = Line.find(Sep + A); Host && F.Agent.empty() && At != std::string::npos; At = Line.find(Sep + A, At + 1))
                    {
                        char After = At + 1 + A.size() < Line.size() ? Line[At + 1 + A.size()] : 0;
                        if(!After || strchr("\\/.-_ \"", After)) { F.Agent = A; }
                    }
                }
            }
        }
        if(F.Pid)
        {
            HANDLE Claude = OpenProcess(PROCESS_SET_INFORMATION, FALSE, F.Pid);
            if(Claude) { RaiseForTyping(Claude); CloseHandle(Claude); }
        }
        S->Finds.push_back(F);
    }
    S->Cpu.swap(Cpu);
    InterlockedExchange(&S->Done, 1);
    SetEvent(S->Wake);
    return 0;
}

static void AppendColor(std::string& Out, u32 Color, bool32 Background)
{
    char Buf[32];
    u32 Tag = COLOR_TAG(Color);
    if(Tag == COLOR_PALETTE)
    {
        u32 I = Color & 0xFF;
        if(I < 8) { snprintf(Buf, sizeof(Buf), ";%u", (Background ? 40 : 30) + I); }
        else if(I < 16) { snprintf(Buf, sizeof(Buf), ";%u", (Background ? 100 : 90) + I - 8); }
        else { snprintf(Buf, sizeof(Buf), ";%u;5;%u", Background ? 48 : 38, I); }
        Out += Buf;
    }
    else if(Tag == COLOR_RGB)
    {
        snprintf(Buf, sizeof(Buf), ";%u;2;%u;%u;%u", Background ? 48 : 38, (Color >> 16) & 0xFF, (Color >> 8) & 0xFF, Color & 0xFF);
        Out += Buf;
    }
}

// The last MaxLines of a tab's main screen (scrollback, then the screen down to its last non-blank
// row) as text with SGR colors and CRLF line ends, ending with a note line. Replayed by cmd's
// `type` when a shell tab comes back after the server restarts. Empty when there's nothing to keep.
static std::string HistoryText(const vt_terminal& T, s32 MaxLines)
{
    // --- the lines, oldest first ---
    std::vector<const cell*> Lines;
    std::vector<s32> Lens;
    for(const std::vector<cell>& L : T.Scrollback) { Lines.push_back(L.data()); Lens.push_back((s32)L.size()); }
    for(s32 Y = 0; Y < T.Rows; ++Y) { Lines.push_back(T.Screens[0].data() + (size_t)Y * T.Cols); Lens.push_back(T.Cols); }
    // Trailing blanks (and blank rows at the end) aren't worth keeping.
    for(size_t I = 0; I < Lines.size(); ++I)
    {
        while(Lens[I] > 0 && (Lines[I][Lens[I] - 1].Codepoint == 0 || Lines[I][Lens[I] - 1].Codepoint == ' ') &&
              !Lines[I][Lens[I] - 1].Bg) { Lens[I] -= 1; }
    }
    s32 End = (s32)Lines.size();
    while(End > 0 && Lens[End - 1] == 0) { End -= 1; }
    s32 Begin = Max(0, End - MaxLines);
    if(Begin >= End) { return std::string(); }

    // --- as text: an SGR whenever the style changes ---
    std::string Out;
    for(s32 I = Begin; I < End; ++I)
    {
        cell Pen = {};
        for(s32 X = 0; X < Lens[I]; ++X)
        {
            cell C = Lines[I][X];
            if(C.Flags & CELL_WIDE_CONT) { continue; }
            if(Pen.Fg != C.Fg || Pen.Bg != C.Bg || Pen.Attr != C.Attr)
            {
                Out += "\x1b[0";
                if(C.Attr & ATTR_BOLD) { Out += ";1"; }
                if(C.Attr & ATTR_DIM) { Out += ";2"; }
                if(C.Attr & ATTR_ITALIC) { Out += ";3"; }
                if(C.Attr & ATTR_UNDERLINE) { Out += ";4"; }
                if(C.Attr & ATTR_INVERSE) { Out += ";7"; }
                if(C.Attr & ATTR_STRIKE) { Out += ";9"; }
                AppendColor(Out, C.Fg, false);
                AppendColor(Out, C.Bg, true);
                Out += "m";
                Pen = C;
            }
            AppendUtf8(Out, C.Codepoint >= 0x20 ? C.Codepoint : ' ');
        }
        Out += "\x1b[0m\r\n";
    }
    Out += "\x1b[0;90m\xE2\x94\x80\xE2\x94\x80 earlier output, before term restarted \xE2\x94\x80\xE2\x94\x80\x1b[0m\r\n"; // ── ... ──
    return Out;
}

// Where a tab's recent output is kept across server restarts: %LOCALAPPDATA%\moss\history\<id>.txt.
static std::wstring HistoryPath(const std::string& Id)
{
    wchar_t Base[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", Base, MAX_PATH);
    return std::wstring(Base) + L"\\moss\\history" + InstanceSuffix() + L"\\" + Utf8ToWide(Id) + L".txt"; // per instance
}

// Saves a shell tab's last lines of output for the next server to show (claude tabs come back by
// --resume instead, so theirs is dropped). UTF-16 with a BOM: cmd's `type` prints that right in
// any code page.
static void SaveHistory(tab& T)
{
    std::string Text = T.SessionId.empty() ? HistoryText(T.Term, 200) : std::string();
    if(Text == T.HistorySaved) { return; }
    T.HistorySaved = Text;
    std::wstring Path = HistoryPath(T.HistoryId);
    if(Text.empty()) { DeleteFileW(Path.c_str()); return; }
    CreateDirectoryW(Path.substr(0, Path.find_last_of(L'\\')).c_str(), 0);
    std::wstring Wide = L"\xFEFF" + Utf8ToWide(Text);
    HANDLE File = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { return; }
    DWORD Written = 0;
    WriteFile(File, Wide.data(), (DWORD)(Wide.size() * sizeof(wchar_t)), &Written, 0);
    CloseHandle(File);
}

// (Re)starts a tab's cmd, running the user's init.bat. With a SessionId, cmd then runs claude:
// resuming when the session file exists, otherwise claiming the id. Claude exiting leaves the shell.
static void StartTab(tab& Tab, s32 Cols, s32 Rows, HANDLE Job, HANDLE Wake, server_state& Server, u64 NowMs)
{
    VtInit(Tab.Term, Cols, Rows);
    Tab.ScrollOffset = 0;
    Tab.SeenPushed = 0;
    Tab.Exited = false;
    Tab.TitleBusy = false;
    Tab.ClaudePid = 0;
    Tab.LaunchedAtMs = 0;
    Tab.SentValid = false;

    std::wstring Cwd = Utf8ToWide(Tab.Cwd);
    if(Cwd.empty() || GetFileAttributesW(Cwd.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        Logf("server", "tab %u: folder missing: %s (starting in %s)", Tab.Id, Tab.Cwd.c_str(), Server.LaunchCwd.c_str());
        Cwd = Utf8ToWide(Server.LaunchCwd);
    }

    // --- the command: cmd /k "call init.bat & clink inject & cd /d <cwd> & claude ..." ---
    // The user's own setup lives in ~\.moss (seeded from term's defaults on first run): init.bat,
    // and the clink profile ~\.moss\clink, where clink itself finds clink_settings, .inputrc and Lua
    // scripts, and keeps history. init.bat runs before the inject, so it can also steer clink.
    // Clink (vendored, next to moss.exe) runs from a DLL cache in %TEMP%, so it never locks its files.
    wchar_t Exe[MAX_PATH] = {};
    if(!GetEnvironmentVariableW(L"ComSpec", Exe, MAX_PATH)) { wcscpy_s(Exe, L"cmd.exe"); }
    std::wstring UserDir = UserTermDir();
    std::wstring Inner;
    std::wstring Init = UserDir + L"init.bat";
    // A shell back after a restart first shows its saved output, above the new prompt (cmd only: it types it).
    std::wstring History = HistoryPath(Tab.HistoryId);
    if(Tab.ReplayHistory && Tab.SessionId.empty() && GetFileAttributesW(History.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        Inner = L"type \"" + History + L"\"";
    }
    Tab.ReplayHistory = false;
    if(GetFileAttributesW(Init.c_str()) != INVALID_FILE_ATTRIBUTES) { Inner += (Inner.empty() ? L"" : L" & ") + std::wstring(L"call \"") + Init + L"\""; }
    std::wstring Clink = ExeDir() + L"clink\\clink_x64.exe";
    if(GetFileAttributesW(Clink.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if(!Inner.empty()) { Inner += L" & "; }
        Inner += L"\"" + Clink + L"\" inject --quiet --profile \"" + UserDir + L"clink\"";
    }
    if(Tab.SessionId.empty() && !Tab.AgentCommand.empty())
    {
        // Another CLI agent: started in the tab's folder once; exiting it leaves the shell.
        if(!Inner.empty()) { Inner += L" & "; }
        Inner += L"cd /d \"" + Cwd + L"\" & " + Tab.AgentCommand;
        Tab.AgentCommand.clear();
    }
    if(!Tab.SessionId.empty())
    {
        if(!Inner.empty()) { Inner += L" & "; }
        std::wstring Claude = FindClaude();
        if(Claude.empty())
        {
            // No Claude Code on this machine: say so in the shell instead of failing obscurely.
            Inner += L"echo. & echo   Claude Code is not installed (no claude.exe or claude.cmd found). Install it, e.g. in PowerShell: & "
                     L"echo. & echo       irm https://claude.ai/install.ps1 ^| iex & echo.";
            Tab.SessionId.clear();
        }
        else
        {
            bool32 Resume = !SessionFilePath(Tab.SessionId).empty();
            Inner += L"cd /d \"" + Cwd + L"\" & " + Claude + (Resume ? L" --resume " : L" --session-id ") + Utf8ToWide(Tab.SessionId);
            Tab.LaunchedAtMs = NowMs;
        }
    }
    // cmd strips the outer quotes of a /k string that holds more than one quoted part.
    std::wstring Cmd = std::wstring(L"\"") + Exe + L"\"" + (Inner.empty() ? std::wstring() : L" /k \"" + Inner + L"\"");

    // --- environment: drop the markers a parent Claude Code session leaves behind (ClaudeMarkers) ---
    // Inherited clink config locations are dropped, so another clink setup never leaks into term's.
    static const wchar_t* Strip[] = {
        L"CLINK_SETTINGS", L"CLINK_INPUTRC", L"CLINK_PATH", L"CLINK_PROFILE", L"CLINK_HISTORY_LABEL", L"MOSS_DIR",
    };
    std::wstring Env;
    bool32 Vs = ReadSetting("vs_env") != "off";
    std::vector<bool32> VsDone(Server.VsSet.size(), false);
    wchar_t* Block = GetEnvironmentStringsW();
    for(const wchar_t* Var = Block; *Var; Var += wcslen(Var) + 1)
    {
        const wchar_t* Eq = wcschr(Var + 1, L'='); // +1: skips the leading '=' of "=C:=C:\..." entries
        size_t NameLen = Eq ? (size_t)(Eq - Var) : wcslen(Var);
        bool32 Drop = false;
        for(const wchar_t* S : Strip) { if(wcslen(S) == NameLen && _wcsnicmp(Var, S, NameLen) == 0) { Drop = true; } }
        for(const wchar_t* S : ClaudeMarkers) { if(wcslen(S) == NameLen && _wcsnicmp(Var, S, NameLen) == 0) { Drop = true; } }
        if(Drop) { continue; }
        // Visual Studio's build environment changes this one: set, or put around its own value.
        std::wstring Entry = Var;
        for(size_t I = 0; Vs && I < Server.VsSet.size(); ++I)
        {
            if(Server.VsSet[I].find(L'=') == NameLen && _wcsnicmp(Var, Server.VsSet[I].c_str(), NameLen) == 0) { Entry = Server.VsSet[I]; VsDone[I] = true; }
        }
        for(const std::wstring& P : Server.VsPrepend)
        {
            if(Vs && P.find(L'=') == NameLen && _wcsnicmp(Var, P.c_str(), NameLen) == 0) { Entry = P + Entry.substr(NameLen + 1); }
        }
        for(const std::wstring& P : Server.VsAppend)
        {
            if(Vs && P.find(L'=') == NameLen && _wcsnicmp(Var, P.c_str(), NameLen) == 0) { Entry += P.substr(NameLen + 1); }
        }
        Env.append(Entry);
        Env.push_back(0);
    }
    FreeEnvironmentStringsW(Block);
    for(size_t I = 0; Vs && I < Server.VsSet.size(); ++I) { if(!VsDone[I]) { Env.append(Server.VsSet[I]); Env.push_back(0); } }
    // Each tab keeps its own clink history (clink_history_<label> in the profile), so up-arrow in a
    // tab brought back after a restart recalls that tab's commands. The label is the tab's lasting
    // id without dashes (clink allows 32 letters and digits): the session, else the shell's history id.
    std::wstring Label;
    for(char Ch : Tab.SessionId.empty() ? Tab.HistoryId : Tab.SessionId) { if(isalnum((u8)Ch) && Label.size() < 32) { Label += (wchar_t)Ch; } }
    Env += L"CLINK_HISTORY_LABEL=" + Label;
    Env.push_back(0);
    // Tab completion for git, npm, winget, ... (vendored clink-completions, next to moss.exe). Clink
    // still loads the profile's own scripts; init.bat can add to CLINK_PATH or clear it.
    std::wstring Completions = ExeDir() + L"clink-completions";
    if(GetFileAttributesW(Completions.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        Env += L"CLINK_PATH=" + Completions;
        Env.push_back(0);
    }
    // Where moss.exe is (with a trailing backslash).
    Env += L"MOSS_DIR=" + ExeDir();
    Env.push_back(0);
    Env.push_back(0);

    std::string Error;
    Tab.Pty = PtyStart(Cmd, Cwd, Env.c_str(), Cols, Rows, Job, Wake, Error);
    if(!Tab.Pty)
    {
        Tab.Exited = true;
        Logf("server", "tab %u: %s", Tab.Id, Error.c_str());
    }
    Logf("server", "tab %u started: %s in %s, %dx%d%s", Tab.Id, Tab.SessionId.empty() ? "shell" : "claude", WideToUtf8(Cwd).c_str(), Cols, Rows,
         Tab.SessionId.empty() ? "" : (" session " + Tab.SessionId).c_str());
}

// Shell: a plain cmd. Otherwise claude runs in it, with SessionId or a brand new one.
static tab* OpenTab(server_state& Server, bool32 Shell, const std::string& SessionId, const std::string& HistoryId, const std::string& Cwd,
                    const std::string& Title, s32 Cols, s32 Rows, u32 Request, const std::string& AgentCommand, HANDLE Job,
                    HANDLE Wake, u64 NowMs)
{
    tab* T = new tab();
    T->AgentCommand = Utf8ToWide(AgentCommand);
    T->Id = ++Server.NextId;
    T->Request = Request;
    T->SessionId = Shell ? std::string() : (SessionId.empty() ? NewUuid() : SessionId);
    T->Cwd = Cwd;
    T->Project = ProjectName(Cwd);
    T->Title = Title;
    // A shell restored after a restart shows its saved output again (when there is any).
    T->HistoryId = HistoryId.empty() ? NewUuid() : HistoryId;
    T->ReplayHistory = !HistoryId.empty();
    StartTab(*T, Max(Cols, 2), Max(Rows, 2), Job, Wake, Server, NowMs);
    Server.Tabs.push_back(T);
    return T;
}

static void CloseTab(server_state& Server, tab* T)
{
    auto It = std::find(Server.Tabs.begin(), Server.Tabs.end(), T);
    if(It == Server.Tabs.end()) { return; }
    Logf("server", "tab %u closed", T->Id);
    DeleteFileW(HistoryPath(T->HistoryId).c_str()); // a closed tab's output isn't coming back
    PtyClose(T->Pty);
    delete T;
    Server.Tabs.erase(It);
}

// A program's whole output (stdout and stderr), run hidden; empty if it can't start. Input goes to
// its stdin; Env (a block of "NAME=value\0" entries) replaces our environment when given; Dir is its
// folder (empty = ours). A program still running after TimeoutMs is ended, with what it wrote so far.
static std::string RunCapture(std::wstring Cmd, const std::string& Input, const std::wstring& Env, const std::wstring& Dir, u32 TimeoutMs)
{
    SECURITY_ATTRIBUTES Sa = {sizeof(Sa), 0, TRUE};
    HANDLE Read = 0;
    HANDLE Write = 0;
    HANDLE InRead = 0;
    HANDLE InWrite = 0;
    if(!CreatePipe(&Read, &Write, &Sa, 0)) { return std::string(); }
    // The stdin pipe holds all of Input, so writing it never waits on the child (the timeout below
    // starts after).
    if(!CreatePipe(&InRead, &InWrite, &Sa, (DWORD)Max((s32)Input.size() + 1, 1 << 16))) { CloseHandle(Read); CloseHandle(Write); return std::string(); }
    SetHandleInformation(Read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(InWrite, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW Si = {};
    Si.cb = sizeof(Si);
    Si.dwFlags = STARTF_USESTDHANDLES;
    Si.hStdInput = InRead;
    Si.hStdOutput = Write;
    Si.hStdError = Write;
    PROCESS_INFORMATION Pi = {};
    bool32 Started = CreateProcessW(0, &Cmd[0], 0, 0, TRUE, CREATE_NO_WINDOW | (Env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
                                    Env.empty() ? 0 : (void*)Env.data(), Dir.empty() ? 0 : Dir.c_str(), &Si, &Pi);
    CloseHandle(Write);
    CloseHandle(InRead);
    DWORD Wrote = 0;
    if(Started && !Input.empty()) { WriteFile(InWrite, Input.data(), (DWORD)Input.size(), &Wrote, 0); }
    CloseHandle(InWrite);
    // --- read until it exits (or runs out of time); a child it started may hold the pipe open longer ---
    std::string Out;
    char Buf[4096];
    u64 StartMs = GetTickCount64();
    while(Started)
    {
        DWORD Avail = 0;
        DWORD Got = 0;
        if(!PeekNamedPipe(Read, 0, 0, 0, &Avail, 0)) { break; } // closed: everything's read
        if(Avail && ReadFile(Read, Buf, Min((s32)Avail, (s32)sizeof(Buf)), &Got, 0)) { Out.append(Buf, Got); continue; }
        if(WaitForSingleObject(Pi.hProcess, 50) == WAIT_OBJECT_0)
        {
            while(PeekNamedPipe(Read, 0, 0, 0, &Avail, 0) && Avail && ReadFile(Read, Buf, Min((s32)Avail, (s32)sizeof(Buf)), &Got, 0)) { Out.append(Buf, Got); }
            break;
        }
        if(GetTickCount64() - StartMs > TimeoutMs) { TerminateProcess(Pi.hProcess, 1); break; }
    }
    if(Started)
    {
        CloseHandle(Pi.hProcess);
        CloseHandle(Pi.hThread);
    }
    CloseHandle(Read);
    return Out;
}

// Visual Studio's x64 build environment for the tabs, so cl, link, cmake, ninja and msbuild work in
// any shell: what vcvars64.bat of the newest install with the C++ tools changes, as a difference from
// our own environment (set, put in front of, put after). Running it takes seconds, so the result is
// kept in %LOCALAPPDATA%\moss\vsenv.txt and only run again when a Visual Studio folder it names is
// gone (an update moved the tools). Nothing when there's no such install, or when we were started from
// a developer prompt already (the tabs inherit that).
static void LoadVsEnv(server_state& Server)
{
    TIMED_BLOCK(LoadVsEnv);
    if(GetEnvironmentVariableW(L"VSINSTALLDIR", 0, 0)) { return; }
    wchar_t Local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", Local, MAX_PATH);
    std::wstring CachePath = std::wstring(Local) + L"\\moss\\vsenv.txt";

    // --- the cache: "set|pre|post <tab> NAME=value" lines, kept while every VS folder in it exists ---
    std::string Cached = ReadWholeFile(CachePath);
    bool32 Valid = !Cached.empty();
    std::vector<std::wstring> Lists[3];
    for(size_t At = 0; At < Cached.size() && Valid;)
    {
        size_t End = Cached.find('\n', At);
        if(End == std::string::npos) { End = Cached.size(); }
        std::string Line = Cached.substr(At, End - At);
        At = End + 1;
        while(!Line.empty() && Line.back() == '\r') { Line.pop_back(); }
        size_t Tab = Line.find('\t');
        if(Tab == std::string::npos) { continue; }
        std::string Kind = Line.substr(0, Tab);
        std::wstring Entry = Utf8ToWide(Line.substr(Tab + 1));
        s32 List = (Kind == "set") ? 0 : (Kind == "pre") ? 1 : (Kind == "post") ? 2 : -1;
        if(List < 0) { continue; }
        Lists[List].push_back(Entry);
        // Folders of Visual Studio named in a list value must still be there.
        std::wstring Value = Entry.substr(Entry.find(L'=') + 1);
        for(size_t P = 0; P < Value.size() && Valid;)
        {
            size_t Semi = Value.find(L';', P);
            if(Semi == std::wstring::npos) { Semi = Value.size(); }
            std::wstring Dir = Value.substr(P, Semi - P);
            P = Semi + 1;
            if(Dir.find(L"Microsoft Visual Studio") != std::wstring::npos && GetFileAttributesW(Dir.c_str()) == INVALID_FILE_ATTRIBUTES) { Valid = false; }
        }
    }

    if(!Valid)
    {
        // --- find the newest install with the C++ x64 tools, run its vcvars64.bat, read back `set` ---
        Logf("server", "vs env: running vcvars64.bat (no valid %s)", WideToUtf8(CachePath).c_str());
        for(std::vector<std::wstring>& L : Lists) { L.clear(); }
        wchar_t X86[MAX_PATH] = {};
        if(!GetEnvironmentVariableW(L"ProgramFiles(x86)", X86, MAX_PATH)) { return; }
        std::wstring Vswhere = std::wstring(X86) + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe";
        if(GetFileAttributesW(Vswhere.c_str()) == INVALID_FILE_ATTRIBUTES) { return; }
        std::string Install = RunCapture(L"\"" + Vswhere + L"\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath", std::string(), std::wstring(), std::wstring(), 60000);
        while(!Install.empty() && strchr("\r\n ", Install.back())) { Install.pop_back(); }
        std::wstring Vcvars = Utf8ToWide(Install) + L"\\VC\\Auxiliary\\Build\\vcvars64.bat";
        if(Install.empty() || GetFileAttributesW(Vcvars.c_str()) == INVALID_FILE_ATTRIBUTES) { return; }
        // cmd /u writes `set`'s output as UTF-16.
        std::string Raw = RunCapture(L"cmd.exe /d /u /c \"\"" + Vcvars + L"\" >nul 2>&1 && set\"", std::string(), std::wstring(), std::wstring(), 120000);
        std::wstring Text((const wchar_t*)Raw.data(), Raw.size() / sizeof(wchar_t));
        for(size_t At = 0; At < Text.size();)
        {
            size_t End = Text.find(L'\n', At);
            if(End == std::wstring::npos) { End = Text.size(); }
            std::wstring Line = Text.substr(At, End - At);
            At = End + 1;
            while(!Line.empty() && Line.back() == L'\r') { Line.pop_back(); }
            size_t Eq = Line.find(L'=');
            if(Eq == std::wstring::npos || Eq == 0) { continue; }
            std::wstring Name = Line.substr(0, Eq);
            std::wstring New = Line.substr(Eq + 1);
            DWORD Len = GetEnvironmentVariableW(Name.c_str(), 0, 0);
            std::wstring Old(Len, 0);
            if(Len) { Old.resize(GetEnvironmentVariableW(Name.c_str(), &Old[0], Len)); }
            if(New == Old) { continue; }
            size_t Pos = Old.empty() ? std::wstring::npos : New.find(Old);
            if(Pos == std::wstring::npos) { Lists[0].push_back(Name + L"=" + New); continue; }
            if(Pos > 0) { Lists[1].push_back(Name + L"=" + New.substr(0, Pos)); }
            if(Pos + Old.size() < New.size()) { Lists[2].push_back(Name + L"=" + New.substr(Pos + Old.size())); }
        }
        std::string Out;
        static const char* Kinds[3] = {"set", "pre", "post"};
        for(s32 K = 0; K < 3; ++K) { for(const std::wstring& E : Lists[K]) { Out += std::string(Kinds[K]) + "\t" + WideToUtf8(E) + "\n"; } }
        CreateDirectoryW((std::wstring(Local) + L"\\moss").c_str(), 0);
        FILE* File = 0;
        _wfopen_s(&File, CachePath.c_str(), L"wb");
        if(File) { fwrite(Out.data(), 1, Out.size(), File); fclose(File); }
    }
    Server.VsSet = Lists[0];
    Server.VsPrepend = Lists[1];
    Server.VsAppend = Lists[2];
}

// The command a shell's title says is running ("<path>\cmd.exe - ping  -n 3 x", spaces tidied), or
// empty: at the prompt, a title some program set itself, or one of the steps term itself has cmd
// run at startup (replay saved output, init.bat, clink, cd, claude by full path).
static std::string RunningCommand(const std::string& Title)
{
    size_t Exe = ToLower(Title).find("cmd.exe - ");
    if(Exe == std::string::npos) { return std::string(); }
    std::string Command;
    for(char C : Title.substr(Exe + 10)) { if(!(C == ' ' && (Command.empty() || Command.back() == ' '))) { Command += C; } }
    while(!Command.empty() && Command.back() == ' ') { Command.pop_back(); }
    bool32 Ours = Command.compare(0, 6, "type \"") == 0 || Command.compare(0, 6, "call \"") == 0 || Command.compare(0, 7, "cd /d \"") == 0 ||
                  Command.find("clink_x64.exe") != std::string::npos ||
                  (!Command.empty() && Command[0] == '"' && (Command.find(" --session-id ") != std::string::npos || Command.find(" --resume ") != std::string::npos));
    return Ours ? std::string() : Command;
}

static std::string TabName(const tab& Tab)
{
    // A shell: cmd titles itself "<path>\cmd.exe - <command>" while a command runs, and just its exe
    // path at the prompt. Show the running command, else the full path it's in (it follows cd); a
    // title some program set itself (title, vim) shows as is. Only the folder, at the prompt, gets the
    // prompt mark (the way claude's own titles start with its ✳), so ❯ means "the shell is yours".
    if(Tab.SessionId.empty())
    {
        const char* Mark = "\xE2\x9D\xAF "; // ❯
        if(!Tab.Term.Title.empty() && ToLower(Tab.Term.Title).find("cmd.exe") == std::string::npos) { return Tab.Term.Title; }
        std::string Running = RunningCommand(Tab.Term.Title);
        if(!Running.empty()) { return Running; }
        return Mark + (!Tab.Cwd.empty() ? Tab.Cwd : Tab.Project); // the full path it's in
    }
    std::string Fallback = !Tab.Title.empty() ? Tab.Title : (!Tab.Project.empty() ? Tab.Project : "claude");
    std::string Name = Tab.Term.Title;
    if(Name.empty()) { return Fallback; }
    size_t Generic = Name.find("Claude Code");
    if(Generic != std::string::npos && !Tab.Title.empty()) { Name.replace(Generic, 11, Tab.Title); }
    return Name;
}

// Keeps a scrolled-back view on the same lines while output arrives (and inside the scrollback).
static void TabSyncScroll(tab& Tab)
{
    if(Tab.ScrollOffset > 0)
    {
        Tab.ScrollOffset += (s32)(Tab.Term.ScrollbackPushed - Tab.SeenPushed);
        Tab.ScrollOffset = Clamp(Tab.ScrollOffset, 0, Tab.Term.AltActive ? 0 : (s32)Tab.Term.Scrollback.size());
    }
    Tab.SeenPushed = Tab.Term.ScrollbackPushed;
}

// The window's tab list: msg_tabs, then each tab's msg_tab and its strings.
static std::string EncodeTabs(const server_state& Server)
{
    std::string Out;
    msg_tabs H = {(u32)Server.Tabs.size()};
    Out.append((const char*)&H, sizeof(H));
    for(const tab* T : Server.Tabs)
    {
        std::string Key = T->SessionId.empty() ? T->HistoryId : T->SessionId;
        std::string Name = TabName(*T);
        msg_tab M = {};
        M.Id = T->Id;
        M.Request = T->Request;
        std::string Agent = !T->SessionId.empty() ? std::string("claude") : T->Agent;
        M.Flags = (Agent.empty() ? 0 : TAB_AGENT) | (T->TitleBusy ? TAB_BUSY : 0) | (T->NeedsInput ? TAB_NEEDS_INPUT : 0) |
                  (T->Exited ? TAB_EXITED : 0) | (T->Running ? TAB_RUNNING : 0);
        M.Cpu = (float)(s32)(T->Cpu + 0.5f); // whole percents: a change of a tenth doesn't resend the list
        M.KeyLen = (u32)Key.size();
        M.NameLen = (u32)Name.size();
        M.CwdLen = (u32)T->Cwd.size();
        M.NoticeLen = (u32)T->Notice.size();
        M.AgentLen = (u32)Agent.size();
        M.OwnerLen = (u32)T->Owner.size();
        Out.append((const char*)&M, sizeof(M));
        Out += Key;
        Out += Name;
        Out += T->Cwd;
        Out += T->Notice;
        Out += Agent;
        Out += T->Owner;
    }
    return Out;
}

// A frame of one tab for the window: the rows of its view (scrolled back by ScrollOffset) that differ
// from what was sent, or all of them; with its cursor and modes. False when nothing changed.
// Where Want occurs in a row of cells (second halves of wide characters skipped, folded unless Case): each match's
// first and last column.
static void RowMatches(const cell* Row, s32 N, const std::vector<u32>& Want, bool32 Case, std::vector<std::pair<s32, s32>>& Out)
{
    Out.clear();
    if(Want.empty()) { return; }
    for(s32 X = 0; X < N; ++X)
    {
        if(Row[X].Flags & CELL_WIDE_CONT) { continue; }
        s32 C = X;
        s32 Last = X;
        size_t K = 0;
        while(K < Want.size() && C < N)
        {
            if(Row[C].Flags & CELL_WIDE_CONT) { ++C; continue; }
            u32 A = Row[C].Codepoint ? Row[C].Codepoint : ' ';
            if(!Case && A < 0x10000) { A = (u32)towlower((wint_t)A); }
            if(A != Want[K]) { break; }
            Last = C + ((Row[C].Flags & CELL_WIDE) ? 1 : 0);
            ++K;
            ++C;
        }
        if(K == Want.size()) { Out.push_back({X, Last}); X = Last; }
    }
}

static bool32 EncodeTabCells(std::string& Out, tab& T)
{
    const vt_terminal& V = T.Term;
    s32 Cols = V.Cols;
    s32 Rows = V.Rows;
    const std::vector<cell>& Screen = V.Screens[V.AltActive ? 1 : 0];
    s32 SbCount = V.AltActive ? 0 : (s32)V.Scrollback.size();
    s32 Offset = Clamp(T.ScrollOffset, 0, SbCount);
    s32 First = SbCount - Offset;

    msg_cells H = {};
    H.TabId = T.Id;
    H.Cols = Cols;
    H.Rows = Rows;
    H.CursorX = V.CursorX;
    H.CursorY = V.CursorY;
    H.CursorShape = V.CursorShape;
    H.Flags = (V.CursorHidden || Offset > 0 || T.Exited ? CELLS_CURSOR_HIDDEN : 0) | (V.AltActive ? CELLS_ALT_SCREEN : 0) |
              (V.AppCursorKeys ? CELLS_APP_CURSOR_KEYS : 0) | (V.BracketedPaste ? CELLS_BRACKETED_PASTE : 0) |
              (V.FocusEvents ? CELLS_FOCUS_EVENTS : 0) | (V.SyncOutput ? CELLS_SYNC : 0) | (V.MouseSgr ? CELLS_MOUSE_SGR : 0) |
              (V.Win32Input ? CELLS_WIN32_INPUT : 0);
    H.MouseMode = V.MouseMode;
    H.ScrollOffset = Offset;
    H.Scrollback = SbCount;
    bool32 Full = !T.SentValid || T.SentHead.Cols != Cols || T.SentHead.Rows != Rows || T.Sent.size() != (size_t)Cols * Rows;
    if(Full) { T.Sent.assign((size_t)Cols * Rows, cell{}); }

    std::string RowBytes;
    std::vector<cell> Row((size_t)Cols);
    std::vector<std::pair<s32, s32>> Found;
    for(s32 R = 0; R < Rows; ++R)
    {
        // --- the view's row R: a scrollback line (padded with blanks) or a screen row ---
        s32 Line = First + R;
        std::fill(Row.begin(), Row.end(), cell{});
        if(Line < SbCount)
        {
            const std::vector<cell>& L = V.Scrollback[Line];
            s32 N = Min((s32)L.size(), Cols);
            if(N > 0) { memcpy(Row.data(), L.data(), N * sizeof(cell)); }
            if(N > 0 && (Row[N - 1].Flags & CELL_WIDE)) { Row[N - 1] = cell{}; }
        }
        else if(Line - SbCount < Rows)
        {
            memcpy(Row.data(), Screen.data() + (size_t)(Line - SbCount) * Cols, Cols * sizeof(cell));
        }
        // --- the find bar's matches, marked ---
        RowMatches(Row.data(), Cols, T.FindText, T.FindCase, Found);
        s64 Abs = (s64)V.ScrollbackPushed - SbCount + Line;
        for(const std::pair<s32, s32>& M : Found)
        {
            u16 Mark = (u16)((T.FindHas && Abs == T.FindLine && M.first == T.FindCol) ? CELL_FOUND_CURRENT : CELL_FOUND);
            for(s32 X = M.first; X <= M.second && X < Cols; ++X) { Row[X].Flags |= Mark; }
        }
        cell* Was = T.Sent.data() + (size_t)R * Cols;
        if(!Full && memcmp(Was, Row.data(), Cols * sizeof(cell)) == 0) { continue; }
        memcpy(Was, Row.data(), Cols * sizeof(cell));
        RowBytes.append((const char*)&R, sizeof(R));
        RowBytes.append((const char*)Row.data(), Cols * sizeof(cell));
        H.RowCount += 1;
    }
    bool32 HeadChanged = Full || memcmp(&H, &T.SentHead, offsetof(msg_cells, RowCount)) != 0;
    if(!H.RowCount && !HeadChanged) { return false; }
    if(Full) { H.Flags |= CELLS_FULL; }
    Out.append((const char*)&H, sizeof(H));
    Out += RowBytes;
    T.SentHead = H;
    T.SentHead.Flags &= ~CELLS_FULL;
    T.SentHead.RowCount = 0;
    T.SentValid = true;
    return true;
}

// --- open tabs on disk, so a reboot or crash can bring them back with --resume ---

static std::wstring TabsFilePath()
{
    wchar_t Base[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", Base, MAX_PATH);
    std::wstring Dir = std::wstring(Base) + L"\\moss";
    CreateDirectoryW(Dir.c_str(), 0);
    return Dir + L"\\tabs" + InstanceSuffix() + L".txt";
}

// One line per tab: "tab|shell <tab> lasting id <tab> cwd <tab> project <tab> title <tab> owner <tab> program".
// The layout is the window's (session.json): it finds the tabs again by their lasting ids.
static std::string TabsFileContents(const server_state& Server)
{
    std::string Out;
    for(const tab* T : Server.Tabs)
    {
        std::string Fields[6] = {T->SessionId.empty() ? T->HistoryId : T->SessionId, T->Cwd, T->Project, T->Title, T->Owner, T->Program};
        Out += T->SessionId.empty() ? "shell" : "tab";
        for(std::string& F : Fields)
        {
            for(char& C : F) { if(C == '\t' || C == '\n' || C == '\r') { C = ' '; } }
            Out += "\t" + F;
        }
        Out += "\n";
    }
    return Out;
}

static void WriteTabsFile(const std::string& Contents)
{
    std::wstring Path = TabsFilePath();
    std::wstring Temp = Path + L".tmp";
    HANDLE File = CreateFileW(Temp.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { return; }
    DWORD Written = 0;
    WriteFile(File, Contents.data(), (DWORD)Contents.size(), &Written, 0);
    CloseHandle(File);
    MoveFileExW(Temp.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static std::string ReadTabsFile()
{
    HANDLE File = CreateFileW(TabsFilePath().c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { return std::string(); }
    std::string Out = ReadFileRange(File, 0, 1024 * 1024);
    CloseHandle(File);
    return Out;
}

static std::vector<std::string> SplitTabs(const std::string& Line)
{
    std::vector<std::string> Out;
    size_t Start = 0;
    for(;;)
    {
        size_t End = Line.find('\t', Start);
        Out.push_back(Line.substr(Start, End == std::string::npos ? std::string::npos : End - Start));
        if(End == std::string::npos) { return Out; }
        Start = End + 1;
    }
}

// A window attached over the pipe.
struct client
{
    ipc_conn Conn;
    bool32 Attached;          // it sent MSG_ATTACH
    HWND Window;
    std::string Name;         // the window's name (msg_attach): empty = the main window
    u32 FocusedTab;           // its keyboard focus (msg_focus); 0 = none
    std::string SentTabs;     // the tab list it has
    bool32 WantSessions;      // it shows the sessions list (MSG_WANT_SESSIONS)
    std::string SentSessions; // the sessions list it has
    std::string ToSend;       // programs' pass-through output and bells for it, this loop
};

// The main window (the one without a name), else any: where output of tabs no window shows goes.
static client* MainClient(const std::vector<client*>& Clients)
{
    client* Any = 0;
    for(client* C : Clients)
    {
        if(!C->Attached) { continue; }
        if(C->Name.empty()) { return C; }
        if(!Any) { Any = C; }
    }
    return Any;
}

// Sends the reason (and whether the window should close: BYE_*), waits for the window to read it, then hangs up.
static void DropClient(ipc_conn& Client, const std::string& Reason, u8 Bye)
{
    if(!Client.Pipe) { return; }
    Logf("server", "window dropped: %s", Reason.c_str());
    IpcSendString(Client, MSG_BYE, Reason + '\0' + (char)Bye);
    FlushFileBuffers(Client.Pipe);
    DisconnectNamedPipe(Client.Pipe);
    IpcClose(Client);
}

static void StartListening(HANDLE Listen, OVERLAPPED& Ov, HANDLE Event)
{
    Ov = OVERLAPPED{};
    Ov.hEvent = Event;
    ResetEvent(Event);
    if(!ConnectNamedPipe(Listen, &Ov) && GetLastError() == ERROR_PIPE_CONNECTED) { SetEvent(Event); }
}

// The window, from the tray: moss.exe itself (it attaches to this server).
static void LaunchClientWindow()
{
    wchar_t Exe[MAX_PATH] = {};
    GetModuleFileNameW(0, Exe, MAX_PATH);
    std::wstring Cmd = std::wstring(L"\"") + Exe + L"\"";
    wchar_t Home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
    STARTUPINFOW Si = {};
    Si.cb = sizeof(Si);
    PROCESS_INFORMATION Pi = {};
    if(CreateProcessW(0, &Cmd[0], 0, 0, FALSE, 0, 0, Home, &Si, &Pi))
    {
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
    }
}

static tab* FindTab(const server_state& Server, u32 Id)
{
    for(tab* T : Server.Tabs) { if(T->Id == Id) { return T; } }
    return 0;
}

#include "term_tray.cpp"

static int ServerMain()
{
    Debug_Init("server", 0);
    Debug_NameThread("server loop");
    static const char* Phases[] = {"messages", "output", "scans", "send"};
    Debug_SetPhaseNames(Phases, 4);
    HANDLE Listen = IpcCreatePipe(true);
    if(Listen == INVALID_HANDLE_VALUE)
    {
        Logf("server", "another server owns the pipe; exiting");
        return 0;
    }

    // Children die with us, even if we crash.
    HANDLE Job = CreateJobObjectW(0, 0);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits = {};
    Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits));

    RaiseForTyping(GetCurrentProcess());
    HANDLE Wake = CreateEventW(0, FALSE, FALSE, 0);
    HANDLE ConnectEvent = CreateEventW(0, TRUE, FALSE, 0);
    // Wakes the loop exactly when a held-back frame may go out (plain wait timeouts round up to ~16 ms).
    HANDLE FrameTimer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    OVERLAPPED ConnectOv = {};
    StartListening(Listen, ConnectOv, ConnectEvent);
    std::vector<client*> Clients; // the windows attached (each its own process)

    server_state Server = {};
    {
        wchar_t Home[MAX_PATH] = {};
        GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
        Server.LaunchCwd = WideToUtf8(Home);
    }
    SeedUserSetup(); // first run: the user's own copy of the default shell setup and settings
    LoadVsEnv(Server); // before the saved tabs start

    // `term --test` and headless runs: a throwaway instance next to the real one. No tray, no
    // restore file, and it ends together with its window.
    bool32 Ephemeral = GetEnvironmentVariableW(L"MOSS_EPHEMERAL", 0, 0) > 0;
    Logf("server", "instance \"%s\"%s", WideToUtf8(InstanceSuffix()).c_str(), Ephemeral ? ", ephemeral" : "");

    static tray_state Tray = {};
    // MOSS_NO_TRAY: a named test instance that outlives its window (headless --instance) shows no icon.
    bool32 NoTray = Ephemeral || GetEnvironmentVariableW(L"MOSS_NO_TRAY", 0, 0) > 0;
    if(!NoTray) { TrayCreate(Tray, &Server); }

    // --- restore tabs left open by a server that didn't quit cleanly (reboot, crash, kill) ---
    if(!Ephemeral)
    {
        std::string Saved = ReadTabsFile();

        // --- let claudes from the previous server finish exiting first (a quick quit-and-reopen):
        // resumed next to a still-live process of the same session, Claude renames the session ---
        {
            std::vector<std::string> Resuming;
            for(size_t At = 0; At < Saved.size();)
            {
                size_t End = Saved.find('\n', At);
                if(End == std::string::npos) { End = Saved.size(); }
                std::vector<std::string> F = SplitTabs(Saved.substr(At, End - At));
                if(F.size() >= 2 && F[0] == "tab") { Resuming.push_back(F[1]); }
                At = End + 1;
            }
            std::vector<HANDLE> Still;
            WIN32_FIND_DATAW Found = {};
            HANDLE Find = Resuming.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((ClaudeConfigDir() + L"\\sessions\\*.json").c_str(), &Found);
            while(Find != INVALID_HANDLE_VALUE)
            {
                DWORD Pid = (DWORD)_wtoi(Found.cFileName);
                std::string Id;
                std::string Cwd;
                if(Pid && ReadClaudeSession(Pid, Id, Cwd) && std::find(Resuming.begin(), Resuming.end(), Id) != Resuming.end() && Still.size() < MAXIMUM_WAIT_OBJECTS)
                {
                    HANDLE Proc = OpenProcess(SYNCHRONIZE, FALSE, Pid);
                    if(Proc) { Still.push_back(Proc); }
                }
                if(!FindNextFileW(Find, &Found)) { FindClose(Find); break; }
            }
            if(!Still.empty())
            {
                Logf("server", "waiting for %d claude(s) of the previous server to exit", (int)Still.size());
                WaitForMultipleObjects((DWORD)Still.size(), Still.data(), TRUE, 15000);
            }
            for(HANDLE Proc : Still) { CloseHandle(Proc); }
        }
        // A claude tab resumes its session. A shell can't be resumed; it comes back as a fresh one in
        // the same folder (running its program again: nvim and the like), as does a claude that never
        // got a prompt (no session file).
        for(size_t At = 0; At < Saved.size();)
        {
            size_t End = Saved.find('\n', At);
            if(End == std::string::npos) { End = Saved.size(); }
            std::vector<std::string> F = SplitTabs(Saved.substr(At, End - At));
            At = End + 1;
            if(F.size() >= 5 && (F[0] == "tab" || F[0] == "shell"))
            {
                bool32 Claude = (F[0] == "tab" && !SessionFilePath(F[1]).empty());
                std::string Program = !Claude && F.size() >= 7 ? F[6] : std::string();
                tab* T = OpenTab(Server, !Claude, F[1], Claude ? std::string() : F[1], F[2], Claude ? F[4] : std::string(), 120, 40, 0, Program,
                                 Job, Wake, GetTickCount64());
                if(F.size() >= 6) { T->Owner = F[5]; }
                if(!Program.empty()) { Logf("server", "tab %u runs again: %s", T->Id, Program.c_str()); }
            }
        }
        Logf("server", "restored %d tab(s)", (int)Server.Tabs.size());
    }
    std::string SavedTabs = TabsFileContents(Server);
    if(!Ephemeral) { WriteTabsFile(SavedTabs); }

    // --- saved output of tabs that aren't coming back ---
    if(!Ephemeral)
    {
        std::wstring Dir = HistoryPath("*");
        Dir = Dir.substr(0, Dir.find_last_of(L'\\') + 1);
        WIN32_FIND_DATAW Found = {};
        HANDLE Find = FindFirstFileW((Dir + L"*.txt").c_str(), &Found);
        while(Find != INVALID_HANDLE_VALUE)
        {
            std::wstring Name = Found.cFileName;
            bool32 Kept = false;
            for(tab* T : Server.Tabs) { if(Utf8ToWide(T->HistoryId) + L".txt" == Name) { Kept = true; } }
            if(!Kept) { DeleteFileW((Dir + Name).c_str()); }
            if(!FindNextFileW(Find, &Found)) { FindClose(Find); break; }
        }
    }

    u64 StartMs = GetTickCount64();
    u64 LastFrameUs = 0;        // last frame that carried changes (QpcUs)
    u64 LaunchedWindowAtMs = 0; // tray opened a window that hasn't attached yet
    u64 ClaudeScanMs = 0;       // last look for claude processes inside the tabs
    claude_scan* Scan = 0;      // a look under way on its own thread
    std::map<std::pair<DWORD, u64>, u64> CpuKnown; // the tabs' processes' CPU times at the last scan (tab::Cpu)
    u64 CpuAtFt = 0;
    u64 HistorySavedMs = GetTickCount64(); // last save of the shell tabs' output (not at once: restored tabs still have to type theirs)
    std::vector<session_info> Sessions;
    sessions_scan* SessionsScan = 0;
    history_read* HistoryRead = 0;
    u64 SessionsScanMs = 0;
    std::string TrayTip;
    std::string Payload;
    u32 LoopFrame = 0;

    while(!Server.Quit)
    {
        HANDLE Waits[MAXIMUM_WAIT_OBJECTS] = {Wake, ConnectEvent, FrameTimer};
        DWORD WaitCount = 3;
        for(client* C : Clients) { if(C->Conn.Pipe && WaitCount < MAXIMUM_WAIT_OBJECTS - 1) { Waits[WaitCount++] = C->Conn.ReadEvent; } }
        MsgWaitForMultipleObjects(WaitCount, Waits, FALSE, 250, QS_ALLINPUT);
        u64 WorkStartUs = QpcUs();
        u64 PhaseUs[4] = {};
        u64 NowMs = GetTickCount64();
        Debug_SetFrame(++LoopFrame);

        // --- tray window messages ---
        MSG Msg = {};
        while(PeekMessageW(&Msg, 0, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
        if(Tray.Action == TRAY_QUIT)
        {
            Logf("server", "quit from the tray");
            Server.Quit = true; // the tab list stays on disk, so the next launch reopens every tab
        }
        else if(Tray.Action == TRAY_OPEN)
        {
            client* Main = 0;
            for(client* C : Clients) { if(C->Attached && C->Name.empty()) { Main = C; } }
            if(Main)
            {
                // The tray click gives us the right to hand foreground to the window.
                if(Main->Window)
                {
                    if(IsIconic(Main->Window)) { ShowWindow(Main->Window, SW_RESTORE); }
                    if(!SetForegroundWindow(Main->Window)) { FlashWindow(Main->Window, TRUE); }
                }
            }
            else if(NowMs - LaunchedWindowAtMs > 3000)
            {
                LaunchClientWindow();
                LaunchedWindowAtMs = NowMs;
            }
        }
        Tray.Action = TRAY_NONE;
        Tray.ActionTab = 0;
        if(Server.Quit) { break; }

        // --- new connection: one more window (one with the same name as another takes over from it, on attach) ---
        if(WaitForSingleObject(ConnectEvent, 0) == WAIT_OBJECT_0)
        {
            DWORD Ignored = 0;
            if(GetOverlappedResult(Listen, &ConnectOv, &Ignored, FALSE) || GetLastError() == ERROR_PIPE_CONNECTED)
            {
                client* C = new client();
                IpcInit(C->Conn, Listen);
                IpcPump(C->Conn);
                Clients.push_back(C);
                Listen = IpcCreatePipe(false);
            }
            else
            {
                DisconnectNamedPipe(Listen);
            }
            StartListening(Listen, ConnectOv, ConnectEvent);
        }

        // --- window messages ---
        BEGIN_TIMED_BLOCK(Messages);
        for(size_t Ci = 0; Ci < Clients.size(); ++Ci)
        {
            client& C = *Clients[Ci];
            if(!C.Conn.Pipe) { continue; }
            bool32 Alive = IpcPump(C.Conn);
            u32 Type = 0;
            while(IpcNextMessage(C.Conn, Type, Payload))
            {
                u32 Version = 0;
                if(Type == MSG_ATTACH && Payload.size() >= sizeof(Version)) { memcpy(&Version, Payload.data(), sizeof(Version)); }
                if(Type == MSG_ATTACH && (Version != TERM_PROTOCOL_VERSION || Payload.size() < sizeof(msg_attach)))
                {
                    // A window from another build would misread everything that follows.
                    DropClient(C.Conn, "this window is from a different moss build than the running server; quit moss from the tray and open it again", BYE_RECONNECT);
                    C.Attached = false;
                    break;
                }
                if(Type == MSG_ATTACH)
                {
                    // cwd \0 the window's name
                    msg_attach A = {};
                    memcpy(&A, Payload.data(), sizeof(A));
                    std::string Rest = Payload.substr(sizeof(A));
                    size_t Zero = Rest.find('\0');
                    if(Zero != 0 && !Rest.empty()) { Server.LaunchCwd = Rest.substr(0, Zero); }
                    C.Name = Zero == std::string::npos ? std::string() : Rest.substr(Zero + 1);
                    C.Window = (HWND)(uintptr_t)A.Window;
                    C.Attached = true;
                    if(C.Name.empty()) { LaunchedWindowAtMs = 0; }
                    for(tab* T : Server.Tabs) { if(T->Viewer == &C) { T->Viewer = 0; T->SentValid = false; } }
                    for(client* O : Clients)
                    {
                        if(O != &C && O->Attached && O->Name == C.Name) { DropClient(O->Conn, "attached in another window", BYE_CLOSE); O->Attached = false; }
                    }
                    Logf("server", "window %s attached (hwnd 0x%llx)", C.Name.empty() ? "main" : C.Name.c_str(), (unsigned long long)A.Window);
                }
                else if(!C.Attached)
                {
                    continue;
                }
                else if(Type == MSG_WATCH && Payload.size() >= 4)
                {
                    // --- the tabs the window has, the size of the pane each lives in, and which are shown ---
                    u32 Count = 0;
                    memcpy(&Count, Payload.data(), 4);
                    std::vector<msg_watch> W(Min((s32)Count, (s32)((Payload.size() - 4) / sizeof(msg_watch))));
                    if(!W.empty()) { memcpy(W.data(), Payload.data() + 4, W.size() * sizeof(msg_watch)); }
                    std::vector<tab*> Before;
                    for(tab* T : Server.Tabs) { if(T->Viewer == &C) { Before.push_back(T); T->Viewer = 0; } }
                    for(const msg_watch& M : W)
                    {
                        tab* T = FindTab(Server, M.TabId);
                        if(!T) { continue; }
                        bool32 Had = std::find(Before.begin(), Before.end(), T) != Before.end();
                        if(M.Shown && (!Had || T->Viewer)) { T->SentValid = false; } // newly shown, or another window had it
                        if(M.Shown) { T->Viewer = &C; T->Notice.clear(); }
                        s32 Cols = Clamp(M.Cols, 2, 2000);
                        s32 Rows = Clamp(M.Rows, 2, 1000);
                        if(T->Term.Cols == Cols && T->Term.Rows == Rows) { continue; }
                        Logf("server", "tab %u resized %dx%d -> %dx%d", T->Id, T->Term.Cols, T->Term.Rows, Cols, Rows);
                        VtResize(T->Term, Cols, Rows);
                        T->SentValid = false;
                        if(T->Pty)
                        {
                            COORD Size = {(SHORT)Cols, (SHORT)Rows};
                            Conpty().Resize(T->Pty->Console, Size);
                        }
                    }
                }
                else if(Type == MSG_INPUT && Payload.size() > 4)
                {
                    LatencyMark("server_in");
                    u32 Id = 0;
                    memcpy(&Id, Payload.data(), 4);
                    tab* T = FindTab(Server, Id);
                    if(T && T->Pty)
                    {
                        PtyWrite(T->Pty, Payload.substr(4));
                        LatencyMark("server_write");
                        T->NeedsInput = false; // typed into: whatever it asked is being answered
                        if(Payload.find('\r', 4) != std::string::npos) { T->AtPrompt = false; T->EnterAtMs = GetTickCount64(); T->Running = T->PromptSeen && T->SessionId.empty(); } // a command line went in
                        if(T->ScrollOffset) { T->ScrollOffset = 0; T->SentValid = false; }
                    }
                }
                else if(Type == MSG_NEW_TAB && Payload.size() >= sizeof(msg_new_tab))
                {
                    msg_new_tab N = {};
                    memcpy(&N, Payload.data(), sizeof(N));
                    // cwd \0 session \0 agent command
                    std::string Rest = Payload.substr(sizeof(N));
                    std::string Parts[3];
                    for(s32 K = 0, At = 0; K < 3 && At <= (s32)Rest.size(); ++K)
                    {
                        size_t Zero = Rest.find('\0', At);
                        Parts[K] = Rest.substr(At, Zero == std::string::npos ? std::string::npos : Zero - At);
                        At = Zero == std::string::npos ? (s32)Rest.size() + 1 : (s32)Zero + 1;
                    }
                    std::string Cwd = Parts[0].empty() ? Server.LaunchCwd : Parts[0];
                    bool32 Claude = N.Kind == NEW_TAB_CLAUDE;
                    tab* T = OpenTab(Server, !Claude, Parts[1], std::string(), Cwd, std::string(), N.Cols, N.Rows, N.Request,
                                     Claude ? std::string() : Parts[2], Job, Wake, NowMs);   // a shell's: run it first (an editor)
                    T->Owner = C.Name;
                }
                else if(Type == MSG_SET_OWNER && Payload.size() >= 4)
                {
                    // --- a tab moves to another window (torn off, dropped on one, or handed back when one closes) ---
                    u32 Id = 0;
                    memcpy(&Id, Payload.data(), 4);
                    tab* T = FindTab(Server, Id);
                    if(T)
                    {
                        T->Owner = Payload.substr(4);
                        if(T->Viewer && T->Viewer->Name != T->Owner) { T->Viewer = 0; }
                        Logf("server", "tab %u now in window %s", T->Id, T->Owner.empty() ? "main" : T->Owner.c_str());
                    }
                }
                else if(Type == MSG_CLOSE_TAB && Payload.size() >= 4)
                {
                    u32 Id = 0;
                    memcpy(&Id, Payload.data(), 4);
                    tab* T = FindTab(Server, Id);
                    if(T) { CloseTab(Server, T); }
                }
                else if(Type == MSG_SCROLL && Payload.size() >= 8)
                {
                    u32 Id = 0;
                    s32 Lines = 0;
                    memcpy(&Id, Payload.data(), 4);
                    memcpy(&Lines, Payload.data() + 4, 4);
                    tab* T = FindTab(Server, Id);
                    if(T)
                    {
                        s32 MostBack = T->Term.AltActive ? 0 : (s32)T->Term.Scrollback.size();
                        if(Lines == SCROLL_COPY_ALL)
                        {
                            // --- every line as plain text (trailing blanks and blank lines at the end dropped), to
                            //     the window's clipboard the way programs copy (OSC 52) ---
                            const vt_terminal& V = T->Term;
                            std::string Text;
                            size_t Kept = 0;
                            s32 Count = (s32)V.Scrollback.size() + V.Rows;
                            for(s32 L = 0; L < Count; ++L)
                            {
                                const cell* Row = L < (s32)V.Scrollback.size() ? V.Scrollback[(size_t)L].data() : V.Screens[0].data() + (size_t)(L - (s32)V.Scrollback.size()) * V.Cols;
                                s32 N = L < (s32)V.Scrollback.size() ? (s32)V.Scrollback[(size_t)L].size() : V.Cols;
                                bool32 Wrapped = N > 0 && (Row[N - 1].Flags & CELL_WRAPPED);
                                while(!Wrapped && N > 0 && (Row[N - 1].Codepoint == 0 || Row[N - 1].Codepoint == ' ')) { --N; }
                                for(s32 X = 0; X < N; ++X)
                                {
                                    if(Row[X].Flags & CELL_WIDE_CONT) { continue; }
                                    u32 Cp = Row[X].Codepoint ? Row[X].Codepoint : ' ';
                                    char U[4];
                                    s32 B = Cp < 0x80 ? 1 : Cp < 0x800 ? 2 : Cp < 0x10000 ? 3 : 4;
                                    if(B == 1) { U[0] = (char)Cp; }
                                    else if(B == 2) { U[0] = (char)(0xC0 | (Cp >> 6)); U[1] = (char)(0x80 | (Cp & 63)); }
                                    else if(B == 3) { U[0] = (char)(0xE0 | (Cp >> 12)); U[1] = (char)(0x80 | ((Cp >> 6) & 63)); U[2] = (char)(0x80 | (Cp & 63)); }
                                    else { U[0] = (char)(0xF0 | (Cp >> 18)); U[1] = (char)(0x80 | ((Cp >> 12) & 63)); U[2] = (char)(0x80 | ((Cp >> 6) & 63)); U[3] = (char)(0x80 | (Cp & 63)); }
                                    Text.append(U, (size_t)B);
                                }
                                if(!Wrapped) { Text += "\r\n"; }   // a wrapped row goes on in the next: one line
                                if(N > 0) { Kept = Text.size(); }
                            }
                            Text.resize(Kept);
                            static const char* Abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                            std::string B64;
                            for(size_t I = 0; I < Text.size(); I += 3)
                            {
                                u32 V3 = ((u32)(u8)Text[I] << 16) | (I + 1 < Text.size() ? (u32)(u8)Text[I + 1] << 8 : 0) | (I + 2 < Text.size() ? (u32)(u8)Text[I + 2] : 0);
                                B64 += Abc[(V3 >> 18) & 63];
                                B64 += Abc[(V3 >> 12) & 63];
                                B64 += I + 1 < Text.size() ? Abc[(V3 >> 6) & 63] : '=';
                                B64 += I + 2 < Text.size() ? Abc[V3 & 63] : '=';
                            }
                            C.ToSend += "\x1b]52;c;" + B64 + "\x1b\\";
                            Logf("server", "tab %u: %d bytes of text to the clipboard", T->Id, (int)Text.size());
                        }
                        else if(Lines == SCROLL_CLEAR)
                        {
                            T->Term.Scrollback.clear();
                            while(!T->Term.Marks.empty() && T->Term.Marks.front().Line < T->Term.ScrollbackPushed) { T->Term.Marks.pop_front(); }
                            T->ScrollOffset = 0;
                            T->FindHas = false;
                            T->SentValid = false;
                            Logf("server", "tab %u: scrollback cleared", T->Id);
                        }
                        else if(Lines == SCROLL_PREV_MARK || Lines == SCROLL_NEXT_MARK)
                        {
                            // a prompt's line at the top of the view: lines are absolute (pushed + row), the view's
                            // top is pushed - offset
                            u64 Pushed = T->Term.ScrollbackPushed, Top = Pushed - (u64)T->ScrollOffset;
                            s64 Oldest = (s64)Pushed - MostBack;
                            // --- each mark not found yet: the row within 30 lines of it that starts with its folder ---
                            for(vt_terminal::mark& M : T->Term.Marks)
                            {
                                if(M.Exact || (s64)M.Line < Oldest || M.Path.empty()) { continue; }
                                std::vector<u32> Want;
                                for(size_t I = 0; I < M.Path.size();)
                                {
                                    u32 Cp = DecodeUtf8(M.Path.data(), M.Path.size(), &I);
                                    Want.push_back(Cp);
                                }
                                for(s32 D = 0; D <= 30 && !M.Exact; ++D)
                                {
                                    for(s32 Sign = 1; Sign >= -1 && !M.Exact; Sign -= 2)
                                    {
                                        s64 L = (s64)M.Line + Sign * D;
                                        const cell* Row = 0;
                                        s32 N = 0;
                                        if(L >= Oldest && L < (s64)Pushed) { const std::vector<cell>& R = T->Term.Scrollback[(size_t)(L - Oldest)]; Row = R.data(); N = (s32)R.size(); }
                                        else if(L >= (s64)Pushed && L - (s64)Pushed < T->Term.Rows) { Row = &T->Term.Screens[0][(size_t)(L - (s64)Pushed) * T->Term.Cols]; N = T->Term.Cols; }
                                        s32 Need = Min((s32)Want.size(), T->Term.Cols);
                                        bool32 Match = Row && N >= Need && Need > 0;
                                        for(s32 X = 0; X < Need && Match; ++X) { Match = Row[X].Codepoint == Want[(size_t)X]; }
                                        if(Match) { M.Line = (u64)L; M.Exact = true; }
                                        if(D == 0) { break; }
                                    }
                                }
                            }
                            s64 Want = -1;
                            for(const vt_terminal::mark& M : T->Term.Marks)
                            {
                                if((s64)M.Line < Oldest) { continue; } // its line left the scrollback
                                if(Lines == SCROLL_PREV_MARK && M.Line < Top && (Want < 0 || (s64)M.Line > Want)) { Want = (s64)M.Line; }
                                if(Lines == SCROLL_NEXT_MARK && M.Line > Top && (Want < 0 || (s64)M.Line < Want)) { Want = (s64)M.Line; }
                            }
                            if(Want >= 0) { T->ScrollOffset = Want >= (s64)Pushed ? 0 : Clamp((s32)(Pushed - (u64)Want), 0, MostBack); }
                            else if(Lines == SCROLL_NEXT_MARK) { T->ScrollOffset = 0; }
                        }
                        else
                        {
                            T->ScrollOffset = Lines == SCROLL_TO_BOTTOM ? 0 : Clamp(T->ScrollOffset + Lines, 0, MostBack);
                        }
                        T->SeenPushed = T->Term.ScrollbackPushed;
                    }
                }
                else if(Type == MSG_LINK_GET && Payload.size() >= 8)
                {
                    u32 Id = 0;
                    u32 Link = 0;
                    memcpy(&Id, Payload.data(), 4);
                    memcpy(&Link, Payload.data() + 4, 4);
                    tab* T = FindTab(Server, Id);
                    if(T && Link < T->Term.Links.size() && !T->Term.Links[Link].empty())
                    {
                        std::string Reply((const char*)&Id, 4);
                        Reply += T->Term.Links[Link];
                        if(!IpcSendString(C.Conn, MSG_LINK, Reply)) { IpcClose(C.Conn); C.Attached = false; }
                    }
                }
                else if(Type == MSG_HISTORY_GET && Payload.size() >= 4 && !HistoryRead)
                {
                    // --- a claude's history: read on its own thread, sent when it's done ---
                    u32 Id = 0;
                    memcpy(&Id, Payload.data(), 4);
                    tab* T = FindTab(Server, Id);
                    HistoryRead = new history_read();
                    HistoryRead->TabId = Id;
                    HistoryRead->SessionId = T ? T->SessionId : std::string();
                    HistoryRead->Wake = Wake;
                    HANDLE Thread = CreateThread(0, 0, HistoryReadThread, HistoryRead, 0, 0);
                    if(Thread) { CloseHandle(Thread); }
                    else { delete HistoryRead; HistoryRead = 0; }
                }
                else if(Type == MSG_FIND && Payload.size() >= 8)
                {
                    // --- the find bar: the text to mark, and a step to the match before or after the current one ---
                    u32 Id = 0;
                    u32 Flags = 0;
                    memcpy(&Id, Payload.data(), 4);
                    memcpy(&Flags, Payload.data() + 4, 4);
                    tab* T = FindTab(Server, Id);
                    if(T)
                    {
                        T->FindCase = (Flags & FIND_CASE) != 0;
                        T->FindText.clear();
                        for(size_t I = 8; I < Payload.size();)
                        {
                            u32 Cp = DecodeUtf8(Payload.data(), Payload.size(), &I);
                            T->FindText.push_back(!T->FindCase && Cp < 0x10000 ? (u32)towlower((wint_t)Cp) : Cp);
                        }
                        // every match, oldest first, as (absolute line, first column)
                        vt_terminal& V = T->Term;
                        const std::vector<cell>& Screen = V.Screens[V.AltActive ? 1 : 0];
                        s64 Pushed = (s64)V.ScrollbackPushed;
                        s32 Sb = V.AltActive ? 0 : (s32)V.Scrollback.size();
                        std::vector<std::pair<s64, s32>> All;
                        std::vector<std::pair<s32, s32>> InRow;
                        for(s64 L = Pushed - Sb; L < Pushed + V.Rows && All.size() < 100000; ++L)
                        {
                            const cell* Row = 0;
                            s32 N = 0;
                            if(L < Pushed) { const std::vector<cell>& R = V.Scrollback[(size_t)(L - (Pushed - Sb))]; Row = R.data(); N = (s32)R.size(); }
                            else { Row = Screen.data() + (size_t)(L - Pushed) * V.Cols; N = V.Cols; }
                            RowMatches(Row, N, T->FindText, T->FindCase, InRow);
                            for(const std::pair<s32, s32>& M : InRow) { All.push_back({L, M.first}); }
                        }
                        // the current one: a step from where it was (the first time from the bottom of the view, upward)
                        s32 Offset = V.AltActive ? 0 : Clamp(T->ScrollOffset, 0, Sb);
                        std::pair<s64, s32> From = T->FindHas ? std::make_pair(T->FindLine, T->FindCol) : std::make_pair(Pushed - Offset + V.Rows, 0);
                        s64 Index = -1;
                        if(Flags & FIND_NEWER)
                        {
                            for(size_t K = 0; K < All.size() && Index < 0; ++K) { if(All[K] > From) { Index = (s64)K; } }
                            if(Index < 0 && !All.empty()) { Index = 0; }
                        }
                        else
                        {
                            bool32 Stay = T->FindHas && !(Flags & FIND_OLDER);
                            for(size_t K = All.size(); K-- > 0 && Index < 0;) { if(All[K] < From || (Stay && All[K] == From)) { Index = (s64)K; } }
                            if(Index < 0 && !All.empty()) { Index = (s64)All.size() - 1; }
                        }
                        T->FindHas = Index >= 0;
                        if(T->FindHas)
                        {
                            T->FindLine = All[(size_t)Index].first;
                            T->FindCol = All[(size_t)Index].second;
                            // scrolled into view, a third of the way down
                            s64 Top = Pushed - Offset;
                            if(!V.AltActive && (T->FindLine < Top || T->FindLine >= Top + V.Rows))
                            {
                                T->ScrollOffset = Clamp((s32)(Pushed - (T->FindLine - V.Rows / 3)), 0, Sb);
                                T->SeenPushed = V.ScrollbackPushed;
                            }
                        }
                        u32 State[3] = {Id, (u32)All.size(), (u32)(Index + 1)};
                        std::string Reply((const char*)State, sizeof(State));
                        // where they are, for the scrollbar: one per 1/512 of the lines
                        s64 Total = Max(Sb + V.Rows, 1);
                        s32 LastSlot = -1;
                        for(const std::pair<s64, s32>& M : All)
                        {
                            s32 Slot = (s32)((M.first - (Pushed - Sb)) * 512 / Total);
                            if(Slot == LastSlot) { continue; }
                            LastSlot = Slot;
                            u16 At = (u16)Min((s32)((M.first - (Pushed - Sb)) * 65535 / Total), 65535);
                            Reply.append((const char*)&At, 2);
                        }
                        if(!IpcSendString(C.Conn, MSG_FIND_STATE, Reply)) { IpcClose(C.Conn); C.Attached = false; }
                    }
                }
                else if(Type == MSG_WANT_SESSIONS && Payload.size() >= 4)
                {
                    u32 On = 0;
                    memcpy(&On, Payload.data(), 4);
                    C.WantSessions = On != 0;
                    if(C.WantSessions) { SessionsScanMs = 0; } // read them now
                }
                else if(Type == MSG_DEBUG && Payload.size() >= 4)
                {
                    u32 What = 0;
                    memcpy(&What, Payload.data(), 4);
                    Logf("server", "debug request %u from the window", What);
                    if(What == DEBUG_CRASH) { Debug_Crash(); }
                    if(What == DEBUG_QUIT) { Server.Quit = true; }
                }
                else if(Type == MSG_FOCUS && Payload.size() >= 4)
                {
                    // --- focus follows the window's focused pane, for children that asked for focus reports ---
                    u32 Id = 0;
                    memcpy(&Id, Payload.data(), 4);
                    if(Id != C.FocusedTab)
                    {
                        tab* Old = FindTab(Server, C.FocusedTab);
                        tab* New = FindTab(Server, Id);
                        if(Old && Old->Pty && Old->Term.FocusEvents) { PtyWrite(Old->Pty, "\x1b[O"); }
                        if(New && New->Pty && New->Term.FocusEvents) { PtyWrite(New->Pty, "\x1b[I"); }
                        C.FocusedTab = Id;
                    }
                }
            }
            if(!Alive)
            {
                // Window closed: tabs keep running (a test instance ends with its windows instead).
                Logf("server", "window %s gone", C.Name.empty() ? "main" : C.Name.c_str());
                IpcClose(C.Conn);
                C.Attached = false;
            }
        }
        // --- windows gone (closed, dropped, or a failed send) ---
        for(size_t Ci = 0; Ci < Clients.size();)
        {
            client* C = Clients[Ci];
            if(C->Conn.Pipe) { ++Ci; continue; }
            for(tab* T : Server.Tabs) { if(T->Viewer == C) { T->Viewer = 0; } }
            delete C;
            Clients.erase(Clients.begin() + (s64)Ci);
            if(Ephemeral && Clients.empty()) { Server.Quit = true; }
        }
        END_TIMED_BLOCK(Messages);
        PhaseUs[0] = QpcUs() - WorkStartUs;

        // Nothing running and nobody watching: nothing left to serve. The grace period covers
        // the gap between a window spawning this server and connecting to it.
        if(Clients.empty() && Server.Tabs.empty() && NowMs - StartMs > 5000)
        {
            Logf("server", "no tabs and no window: exiting");
            Server.Quit = true;
            break;
        }

        // --- child output into emulators ---
        BEGIN_TIMED_BLOCK(Output);
        u64 OutputStartUs = QpcUs();
        std::string Chunk;
        std::vector<tab*> Ended;
        for(tab* T : Server.Tabs)
        {
            if(!T->Pty) { continue; }
            EnterCriticalSection(&T->Pty->Lock);
            Chunk.swap(T->Pty->Pending);
            LeaveCriticalSection(&T->Pty->Lock);
            std::string Why;
            if(!Chunk.empty())
            {
                bool32 WasSync = T->Term.SyncOutput;
                VtFeed(T->Term, (const u8*)Chunk.data(), Chunk.size());
                Chunk.clear();
                PtyWrite(T->Pty, T->Term.Reply);
                T->Term.Reply.clear();
                client* To = T->Viewer ? T->Viewer : MainClient(Clients);
                if(To) { To->ToSend += T->Term.PassThrough; }
                T->Term.PassThrough.clear();
                // `view <file>` in the tab (OSC 7777;view;<path>): the window shows it over the tab
                if(!T->Term.View.empty() && To) { To->ToSend += "\x1b]7777;view;" + std::to_string(T->Id) + ";" + T->Term.View + "\x1b\\"; }
                T->Term.View.clear();
                TabSyncScroll(*T);
                if(T->Term.SyncOutput && !WasSync) { T->SyncSinceMs = NowMs; }

                // --- reasons to notify: bell, OSC 9/777, or claude's title spinner stopping ---
                size_t TitleAt = 0;
                u32 First = T->Term.Title.empty() ? 0 : DecodeUtf8(T->Term.Title.data(), T->Term.Title.size(), &TitleAt);
                bool32 Busy = (First >= 0x25D0 && First <= 0x25D3); // ◐◑◒◓
                if(T->TitleBusy && !Busy && First == 0x2733) { Why = "Claude finished"; } // ✳
                if(Busy && !T->TitleBusy) { T->NeedsInput = false; } // a new turn: whatever it asked is settled
                T->TitleBusy = Busy;
                if(T->Term.Bell)
                {
                    if(To) { To->ToSend += "\x07"; }
                    T->Term.Bell = false;
                    Why = T->SessionId.empty() ? "Bell" : "Claude needs your attention";
                }
                if(!T->Term.Notify.empty())
                {
                    Why = T->Term.Notify;
                    T->Term.Notify.clear();
                }
                // the prompt says where the shell is (OSC 9;9) the moment it's there, and that whatever ran has
                // ended; the scan only notices later
                if(!T->Term.Cwd.empty())
                {
                    if(T->SessionId.empty() && T->Term.Cwd != T->Cwd) { T->Cwd = T->Term.Cwd; T->Project = ProjectName(T->Cwd); }
                    if(T->SessionId.empty()) { T->Running = false; T->Program.clear(); }
                    T->PromptSeen = true;
                    T->AtPrompt = true;
                    T->Term.Cwd.clear();
                }
            }
            // cmd ending (exit typed in the shell) ends the tab.
            if(!T->Exited && WaitForSingleObject(T->Pty->Process, 0) == WAIT_OBJECT_0)
            {
                T->Exited = true;
                Ended.push_back(T);
            }

            // Unseen: no pane of any window shows the tab, or the window showing it is behind others.
            bool32 Unseen = !T->Viewer || (T->Viewer->Window && GetForegroundWindow() != T->Viewer->Window);
            // Claude ringing or notifying (a permission prompt, a question) waits for the user until
            // something is typed into it or a turn starts.
            if(!T->SessionId.empty() && !Why.empty() && Why != "Claude finished") { T->NeedsInput = true; }
            // Off screen it waits with a notice until some pane shows it.
            if(!Why.empty() && !T->Viewer)
            {
                T->Notice = (Why == "Claude finished") ? "finished" : (Why == "Claude needs your attention") ? "needs you" : (Why == "Bell") ? "bell" : Why;
            }
            if(!Why.empty()) { Logf("server", "tab %u: %s", T->Id, Why.c_str()); }
            if(!Why.empty() && Unseen && NowMs - T->NotifiedAtMs > 10000)
            {
                if(ReadSetting("notifications") != "off") { TrayNotify(Tray, TabName(*T), Why, (s32)T->Id); }
                T->NotifiedAtMs = NowMs;
            }
        }
        for(tab* T : Ended) { CloseTab(Server, T); }
        END_TIMED_BLOCK(Output);
        PhaseUs[1] = QpcUs() - OutputStartUs;

        // --- which tabs are running claude (ClaudeScanThread) ---
        // A snapshot of every process takes tens of milliseconds, so it runs on its own thread and
        // the loop (and typing) never waits for it; its answer is applied once it's in.
        u64 ScansStartUs = QpcUs();
        if(Scan && Scan->Done)
        {
            // Each tab's CPU, as a share of the whole machine since the last scan.
            u64 Elapsed = Scan->PrevAtFt && Scan->AtFt > Scan->PrevAtFt ? Scan->AtFt - Scan->PrevAtFt : 0;
            u64 Cores = Max((s32)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1);
            CpuKnown.swap(Scan->Cpu);
            CpuAtFt = Scan->AtFt;
            for(tab* T : Server.Tabs)
            {
                if(!T->Pty || T->Exited) { continue; }
                const claude_find* Find = 0;
                for(const claude_find& F : Scan->Finds) { if(F.CmdPid == GetProcessId(T->Pty->Process)) { Find = &F; } }
                if(!Find) { continue; } // opened after the scan started
                if(Elapsed) { T->Cpu = (float)(Find->CpuUsed * 100.0 / ((double)Elapsed * Cores)); }
                // a program under the cmd; or just after Enter while the prompt isn't back (the scan may predate it)
                T->Running = T->SessionId.empty() && (Find->HasChild || (T->PromptSeen && !T->AtPrompt && GetTickCount64() - T->EnterAtMs < 3000));
                if(T->SessionId.empty() && Find->Program != T->Program)
                {
                    Logf("server", "tab %u: program %s", T->Id, Find->Program.empty() ? "gone" : Find->Program.c_str());
                    T->Program = Find->Program;
                }
                if(Find->Agent != T->Agent && !Find->Pid)
                {
                    Logf("server", "tab %u: agent %s", T->Id, Find->Agent.empty() ? "gone" : Find->Agent.c_str());
                    T->Agent = Find->Agent;
                }
                // A shell's folder follows its cd, so it comes back there after a restart (tabs.txt).
                if(T->SessionId.empty() && !Find->Pid && !Find->ShellCwd.empty() && Find->ShellCwd != T->Cwd)
                {
                    T->Cwd = Find->ShellCwd;
                    T->Project = ProjectName(Find->ShellCwd);
                }
                if(Find->Pid)
                {
                    if(Find->Id != T->SessionId)
                    {
                        Logf("server", "tab %u: claude session %s", T->Id, Find->Id.c_str());
                        T->SessionId = Find->Id;
                        T->Title.clear();
                    }
                    if(!Find->Cwd.empty() && Find->Cwd != T->Cwd) { T->Cwd = Find->Cwd; T->Project = ProjectName(Find->Cwd); }
                    T->ClaudePid = Find->Pid;
                    T->LaunchedAtMs = 0;
                }
                else if(T->ClaudePid || (T->LaunchedAtMs && NowMs - T->LaunchedAtMs > 30000))
                {
                    // Claude left (or the one the server started never showed up): back to a shell.
                    Logf("server", "tab %u: claude gone, a shell again", T->Id);
                    T->ClaudePid = 0;
                    T->LaunchedAtMs = 0;
                    T->SessionId.clear();
                    T->Title.clear();
                    T->TitleBusy = false;
                }
            }
            delete Scan;
            Scan = 0;
        }
        // Every second while the window is in front (busy states, agents starting), every 3 s otherwise.
        bool32 InFront = false;
        for(client* C : Clients) { InFront |= C->Attached && C->Window && GetForegroundWindow() == C->Window; }
        u64 ScanEveryMs = InFront ? 1000 : 3000;
        if(!Scan && !Server.Tabs.empty() && NowMs - ClaudeScanMs >= ScanEveryMs)
        {
            ClaudeScanMs = NowMs;
            Scan = new claude_scan();
            Scan->Wake = Wake;
            Scan->Cpu = CpuKnown;
            Scan->PrevAtFt = CpuAtFt;
            Scan->Agents = SettingNames("agents", "codex gemini aider opencode copilot qwen cursor-agent amp goose crush");
            Scan->Programs = SettingNames("restore_programs", "nvim vim hx micro nano emacs less lazygit htop btop");
            for(tab* T : Server.Tabs) { if(T->Pty && !T->Exited) { Scan->CmdPids.push_back(GetProcessId(T->Pty->Process)); } }
            HANDLE Thread = CreateThread(0, 0, ClaudeScanThread, Scan, 0, 0);
            if(Thread) { CloseHandle(Thread); }
            else { delete Scan; Scan = 0; }
        }

        // --- a claude's history, read: to the windows ---
        if(HistoryRead && HistoryRead->Done)
        {
            for(client* C : Clients)
            {
                if(C->Attached && !IpcSendString(C->Conn, MSG_HISTORY, HistoryRead->Reply)) { IpcClose(C->Conn); C->Attached = false; }
            }
            delete HistoryRead;
            HistoryRead = 0;
        }

        // --- Claude Code's sessions: read every 5 s while the window shows them (on their own thread) ---
        if(SessionsScan && SessionsScan->Done)
        {
            Sessions = SessionsScan->Sessions;
            delete SessionsScan;
            SessionsScan = 0;
        }
        bool32 WantSessions = false;
        for(client* C : Clients) { WantSessions |= C->Attached && C->WantSessions; }
        if(!SessionsScan && WantSessions && NowMs - SessionsScanMs >= 5000)
        {
            SessionsScanMs = NowMs;
            SessionsScan = new sessions_scan();
            SessionsScan->Known = Sessions;
            SessionsScan->Wake = Wake;
            HANDLE Thread = CreateThread(0, 0, SessionsScanThread, SessionsScan, 0, 0);
            if(Thread) { CloseHandle(Thread); }
            else { delete SessionsScan; SessionsScan = 0; }
        }
        std::string SessionList = WantSessions ? EncodeSessions(Sessions) : std::string();
        for(client* C : Clients)
        {
            if(!C->Attached || !C->WantSessions || SessionList == C->SentSessions) { continue; }
            if(!IpcSendString(C->Conn, MSG_SESSIONS, SessionList)) { IpcClose(C->Conn); C->Attached = false; }
            C->SentSessions = SessionList;
        }

        // --- tray tooltip and the restore file follow the tab set ---
        std::string Tip = "moss - " + std::to_string(Server.Tabs.size()) + " tab" + (Server.Tabs.size() == 1 ? "" : "s");
        if(Tip != TrayTip && !NoTray)
        {
            TraySetTip(Tray, Tip);
            TrayTip = Tip;
        }
        std::string TabsText = TabsFileContents(Server);
        if(!Ephemeral && NowMs - HistorySavedMs >= 30000)
        {
            for(tab* T : Server.Tabs) { SaveHistory(*T); } // also covers a crash or reboot
            HistorySavedMs = NowMs;
        }
        if(TabsText != SavedTabs && !Ephemeral)
        {
            WriteTabsFile(TabsText);
            SavedTabs = TabsText;
        }
        PhaseUs[2] = QpcUs() - ScansStartUs;

        // --- to the window: the tab list when it changed, then the shown tabs' cells ---
        BEGIN_TIMED_BLOCK(Send);
        u64 SendStartUs = QpcUs();
        std::string Tabs = Clients.empty() ? std::string() : EncodeTabs(Server);
        for(client* C : Clients)
        {
            if(!C->Attached) { C->ToSend.clear(); continue; }
            if(!C->ToSend.empty() && !IpcSendString(C->Conn, MSG_OUTPUT, C->ToSend)) { IpcClose(C->Conn); C->Attached = false; }
            C->ToSend.clear();
            if(C->Attached && Tabs != C->SentTabs)
            {
                if(!IpcSendString(C->Conn, MSG_TABS, Tabs)) { IpcClose(C->Conn); C->Attached = false; }
                C->SentTabs = Tabs;
            }
        }
        // At most one frame per 8 ms (a flood of output), but a frame after a quiet spell (an echo)
        // goes out at once; frames that changed nothing don't count. A tab mid synchronized update
        // waits for its end (up to 100 ms).
        u64 NowUs = QpcUs();
        bool32 FrameDue = NowUs - LastFrameUs >= 8000;
        bool32 Sent = false;
        bool32 Held = false;
        for(tab* T : Server.Tabs)
        {
            client* C = T->Viewer;
            if(!C || !C->Attached) { continue; }
            if(T->Term.SyncOutput && NowMs - T->SyncSinceMs < 100) { Held = true; continue; }
            if(!FrameDue) { Held = true; continue; }
            std::string Out;
            if(!EncodeTabCells(Out, *T)) { continue; }
            LatencyMark("server_frame");
            if(!IpcSendString(C->Conn, MSG_CELLS, Out)) { IpcClose(C->Conn); C->Attached = false; continue; }
            Sent = true;
        }
        if(Sent) { LastFrameUs = NowUs; }
        if(Held)
        {
            LARGE_INTEGER Due = {};
            Due.QuadPart = -(s64)Max(1000, (s32)(8000 - Min((s32)(NowUs - LastFrameUs), 8000))) * 10; // relative, in 100 ns
            SetWaitableTimer(FrameTimer, &Due, 0, 0, 0, FALSE);
        }
        END_TIMED_BLOCK(Send);
        PhaseUs[3] = QpcUs() - SendStartUs;
        Debug_FrameEnd(QpcUs() - WorkStartUs, PhaseUs);
    }

    // --- shutdown: tell the window, then hang up every child at once so they exit in parallel ---
    // The tab list is left on disk on purpose: the next launch reopens every tab with --resume,
    // and shell tabs show their saved output again.
    Logf("server", "shutting down with %d tab(s)", (int)Server.Tabs.size());
    if(!Ephemeral) { for(tab* T : Server.Tabs) { SaveHistory(*T); } }
    std::string Count = std::to_string(Server.Tabs.size()) + " tab" + (Server.Tabs.size() == 1 ? "" : "s");
    for(client* C : Clients) { DropClient(C->Conn, Ephemeral ? "test instance ended" : "moss quit from the tray; " + Count + " will reopen the next time you run moss", BYE_CLOSE); }
    if(!NoTray) { TrayDestroy(Tray); }
    // Let go of the pipe now: a quick reopen starts the next server at once instead of waiting
    // on this one (which then waits out these claudes before resuming the same sessions).
    CloseHandle(Listen);
    Listen = INVALID_HANDLE_VALUE;
    for(tab* T : Server.Tabs) { if(T->Pty) { Conpty().Close(T->Pty->Console); T->Pty->Console = 0; } }
    // Give each claude a moment to exit on its own (saving, and removing its session file) before
    // the job takes down whatever is left.
    std::vector<HANDLE> Claudes;
    for(tab* T : Server.Tabs)
    {
        HANDLE Proc = T->ClaudePid ? OpenProcess(SYNCHRONIZE, FALSE, T->ClaudePid) : 0;
        if(Proc && Claudes.size() < MAXIMUM_WAIT_OBJECTS) { Claudes.push_back(Proc); }
        else if(Proc) { CloseHandle(Proc); }
    }
    if(!Claudes.empty()) { WaitForMultipleObjects((DWORD)Claudes.size(), Claudes.data(), TRUE, 5000); }
    for(HANDLE Proc : Claudes) { CloseHandle(Proc); }
    for(tab* T : Server.Tabs)
    {
        PtyClose(T->Pty);
        delete T;
    }
    CloseHandle(ConnectEvent);
    CloseHandle(Wake);
    CloseHandle(Job);
    return 0;
}
