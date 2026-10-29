// Discovers Claude Code sessions on disk: <claude dir>/projects/<mangled cwd>/<session id>.jsonl
// Only the head and tail of each file are read; transcripts can be tens of megabytes.

struct session_info
{
    std::wstring Path;
    std::string Id;
    std::string Cwd;
    std::string Project; // last component of Cwd
    std::string Title;
    u64 ModifiedTime;    // FILETIME, 100ns ticks since 1601
    u64 Size;
    std::string ConfigDir; // its account's config folder (accounts.txt); empty = the default account's
};

static std::wstring Utf8ToWide(const std::string& S)
{
    if(S.empty()) { return std::wstring(); }
    s32 N = MultiByteToWideChar(CP_UTF8, 0, S.data(), (int)S.size(), 0, 0);
    std::wstring W(N, 0);
    MultiByteToWideChar(CP_UTF8, 0, S.data(), (int)S.size(), &W[0], N);
    return W;
}

static std::string WideToUtf8(const std::wstring& W)
{
    if(W.empty()) { return std::string(); }
    s32 N = WideCharToMultiByte(CP_UTF8, 0, W.data(), (int)W.size(), 0, 0, 0, 0);
    std::string S(N, 0);
    WideCharToMultiByte(CP_UTF8, 0, W.data(), (int)W.size(), &S[0], N, 0, 0);
    return S;
}

static std::wstring ClaudeConfigDir()
{
    wchar_t Buf[MAX_PATH] = {};
    if(GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", Buf, MAX_PATH) > 0) { return Buf; }
    GetEnvironmentVariableW(L"USERPROFILE", Buf, MAX_PATH);
    return std::wstring(Buf) + L"\\.claude";
}

// Decodes the JSON string starting just after its opening quote. False if it runs off the buffer.
static bool32 JsonDecodeString(const std::string& Buf, size_t At, std::string& Out)
{
    Out.clear();
    for(size_t I = At; I < Buf.size(); ++I)
    {
        char C = Buf[I];
        if(C == '"') { return true; }
        if(C != '\\') { Out += C; continue; }
        if(++I >= Buf.size()) { return false; }
        switch(Buf[I])
        {
            case 'n': Out += '\n'; break;
            case 't': Out += '\t'; break;
            case 'r': Out += '\r'; break;
            case 'b': case 'f': break;
            case 'u':
            {
                if(I + 4 >= Buf.size()) { return false; }
                u32 Cp = (u32)strtoul(Buf.substr(I + 1, 4).c_str(), 0, 16);
                I += 4;
                if(Cp >= 0xD800 && Cp <= 0xDBFF && I + 6 < Buf.size() && Buf[I + 1] == '\\' && Buf[I + 2] == 'u')
                {
                    u32 Lo = (u32)strtoul(Buf.substr(I + 3, 4).c_str(), 0, 16);
                    Cp = 0x10000 + ((Cp - 0xD800) << 10) + (Lo - 0xDC00);
                    I += 6;
                }
                AppendUtf8(Out, Cp);
            } break;
            default: Out += Buf[I]; break;
        }
    }
    return false;
}

// Finds "Key":"value" (first or last occurrence) and decodes the value.
static bool32 JsonFindString(const std::string& Buf, const char* Key, bool32 Last, std::string& Out)
{
    std::string Pattern = std::string("\"") + Key + "\":\"";
    size_t At = Last ? Buf.rfind(Pattern) : Buf.find(Pattern);
    while(At != std::string::npos)
    {
        if(JsonDecodeString(Buf, At + Pattern.size(), Out) && !Out.empty()) { return true; }
        if(Last)
        {
            if(At == 0) { break; }
            At = Buf.rfind(Pattern, At - 1);
        }
        else
        {
            At = Buf.find(Pattern, At + 1);
        }
    }
    return false;
}

static std::string ReadFileRange(HANDLE File, u64 Offset, u32 Size)
{
    std::string Out(Size, 0);
    LARGE_INTEGER Pos = {};
    Pos.QuadPart = (LONGLONG)Offset;
    SetFilePointerEx(File, Pos, 0, FILE_BEGIN);
    DWORD Read = 0;
    ReadFile(File, &Out[0], Size, &Read, 0);
    Out.resize(Read);
    return Out;
}

static std::string OneLine(const std::string& S)
{
    std::string Out;
    bool32 Space = false;
    for(char C : S)
    {
        bool32 IsSpace = (C == '\n' || C == '\r' || C == '\t' || C == ' ');
        if(IsSpace) { Space = !Out.empty(); continue; }
        if(Space) { Out += ' '; Space = false; }
        Out += C;
    }
    return Out;
}

static bool32 ReadSessionInfo(const std::wstring& Path, session_info& Out)
{
    HANDLE File = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { return false; }

    LARGE_INTEGER Size = {};
    GetFileSizeEx(File, &Size);
    FILETIME Mod = {};
    GetFileTime(File, 0, 0, &Mod);

    const u32 HeadSize = 64 * 1024;
    const u32 TailSize = 256 * 1024;
    u64 FileSize = (u64)Size.QuadPart;
    std::string Head = ReadFileRange(File, 0, (u32)(FileSize < HeadSize ? FileSize : HeadSize));
    std::string Tail;
    if(FileSize > HeadSize)
    {
        u64 TailStart = FileSize > TailSize ? FileSize - TailSize : 0;
        Tail = ReadFileRange(File, TailStart, (u32)(FileSize - TailStart));
    }
    CloseHandle(File);
    const std::string& Last = Tail.empty() ? Head : Tail;

    Out.Path = Path;
    Out.Size = FileSize;
    Out.ModifiedTime = ((u64)Mod.dwHighDateTime << 32) | Mod.dwLowDateTime;
    size_t Slash = Path.find_last_of(L"\\/");
    std::wstring Name = Path.substr(Slash + 1);
    Out.Id = WideToUtf8(Name.substr(0, Name.size() - 6)); // strip ".jsonl"

    // --- cwd: sessions can wander between directories; resume needs the one the project folder is named after ---
    std::string ProjectDir = WideToUtf8(Path.substr(0, Slash));
    ProjectDir = ProjectDir.substr(ProjectDir.find_last_of("\\/") + 1);
    // Returns true once Buf holds the matching cwd; the first cwd seen at all is the fallback.
    auto FindProjectCwd = [&](const std::string& Buf) -> bool32
    {
        size_t At = Buf.find("\"cwd\":\"");
        while(At != std::string::npos)
        {
            std::string Cwd;
            if(JsonDecodeString(Buf, At + 7, Cwd) && !Cwd.empty())
            {
                if(Out.Cwd.empty()) { Out.Cwd = Cwd; }
                std::string Mangled = Cwd;
                for(char& C : Mangled) { if(!isalnum((u8)C)) { C = '-'; } }
                if(Mangled == ProjectDir) { Out.Cwd = Cwd; return true; }
            }
            At = Buf.find("\"cwd\":\"", At + 1);
        }
        return false;
    };
    if(!FindProjectCwd(Head) && !FindProjectCwd(Tail) && FileSize > HeadSize)
    {
        // Long sessions can have no cwd in the head and only later directories in the tail, so walk
        // the middle until the original one turns up. Chunks overlap so a match can't straddle them.
        HANDLE Again = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
        if(Again != INVALID_HANDLE_VALUE)
        {
            const u32 Chunk = 1024 * 1024;
            for(u64 At = HeadSize - 4096; At < FileSize; At += Chunk - 4096)
            {
                if(FindProjectCwd(ReadFileRange(Again, At, Chunk))) { break; }
            }
            CloseHandle(Again);
        }
    }
    size_t CwdSlash = Out.Cwd.find_last_of("\\/");
    Out.Project = (CwdSlash == std::string::npos) ? Out.Cwd : Out.Cwd.substr(CwdSlash + 1);

    // --- title: custom > ai-generated > last prompt > first real user prompt ---
    std::string Title;
    if(JsonFindString(Last, "customTitle", true, Title) || JsonFindString(Head, "customTitle", true, Title) ||
       JsonFindString(Last, "aiTitle", true, Title) || JsonFindString(Head, "aiTitle", true, Title) ||
       JsonFindString(Last, "lastPrompt", true, Title))
    {
        Out.Title = OneLine(Title);
    }
    else
    {
        size_t LineStart = 0;
        while(LineStart < Head.size() && Out.Title.empty())
        {
            size_t LineEnd = Head.find('\n', LineStart);
            if(LineEnd == std::string::npos) { LineEnd = Head.size(); }
            std::string Line = Head.substr(LineStart, LineEnd - LineStart);
            LineStart = LineEnd + 1;
            if(Line.find("\"type\":\"user\"") == std::string::npos || Line.find("\"isMeta\":true") != std::string::npos) { continue; }
            std::string Text;
            if(JsonFindString(Line, "content", false, Text) || JsonFindString(Line, "text", false, Text))
            {
                if(Text[0] != '<') { Out.Title = OneLine(Text); }
            }
        }
    }
    return true;
}

// Every Claude Code session with a prompt, newest first. A transcript unchanged since Known read it
// (same path, write time and size) is taken from Known instead of being read again.
// Every account's sessions (ConfigDirs: each account's config folder, "" = the default account's), newest first.
static std::vector<session_info> ScanSessions(const std::vector<session_info>& Known, const std::vector<std::string>& ConfigDirs)
{
    std::map<std::wstring, const session_info*> Cache;
    for(const session_info& S : Known) { Cache[S.Path] = &S; }
    std::vector<session_info> Result;
    std::vector<std::wstring> Seen;
    for(const std::string& ConfigDir : ConfigDirs)
    {
    std::wstring Projects = (ConfigDir.empty() ? ClaudeConfigDir() : Utf8ToWide(ConfigDir)) + L"\\projects";
    bool32 Again = false;
    for(const std::wstring& P : Seen) { Again |= _wcsicmp(P.c_str(), Projects.c_str()) == 0; }
    Seen.push_back(Projects);

    WIN32_FIND_DATAW Dir = {};
    HANDLE DirFind = Again ? INVALID_HANDLE_VALUE : FindFirstFileW((Projects + L"\\*").c_str(), &Dir);
    if(DirFind == INVALID_HANDLE_VALUE) { continue; }
    do
    {
        if(!(Dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || Dir.cFileName[0] == L'.') { continue; }
        std::wstring ProjectDir = Projects + L"\\" + Dir.cFileName;

        WIN32_FIND_DATAW File = {};
        HANDLE FileFind = FindFirstFileW((ProjectDir + L"\\*.jsonl").c_str(), &File);
        if(FileFind == INVALID_HANDLE_VALUE) { continue; }
        do
        {
            std::wstring Path = ProjectDir + L"\\" + File.cFileName;
            u64 Modified = ((u64)File.ftLastWriteTime.dwHighDateTime << 32) | File.ftLastWriteTime.dwLowDateTime;
            u64 Size = ((u64)File.nFileSizeHigh << 32) | File.nFileSizeLow;
            auto Hit = Cache.find(Path);
            if(Hit != Cache.end() && Hit->second->ModifiedTime == Modified && Hit->second->Size == Size)
            {
                Result.push_back(*Hit->second);
                continue;
            }
            session_info Info = {};
            Info.ConfigDir = ConfigDir;
            if(ReadSessionInfo(Path, Info))
            {
                // No prompt means nothing to resume (a session opened and quit before typing anything).
                if(!Info.Title.empty()) { Result.push_back(Info); }
            }
        } while(FindNextFileW(FileFind, &File));
        FindClose(FileFind);
    } while(FindNextFileW(DirFind, &Dir));
    FindClose(DirFind);
    }

    std::sort(Result.begin(), Result.end(), [](const session_info& A, const session_info& B) { return A.ModifiedTime > B.ModifiedTime; });
    return Result;
}

// A session's transcript (<ConfigDir>/projects/<any>/<id>.jsonl), empty when there's none.
static std::wstring SessionFilePath(const std::wstring& ConfigDir, const std::string& Id)
{
    std::wstring Projects = ConfigDir + L"\\projects";
    std::wstring Want = Utf8ToWide(Id) + L".jsonl";
    WIN32_FIND_DATAW Dir = {};
    HANDLE DirFind = FindFirstFileW((Projects + L"\\*").c_str(), &Dir);
    if(Id.empty() || DirFind == INVALID_HANDLE_VALUE) { return std::wstring(); }
    std::wstring Found;
    do
    {
        if(!(Dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || Dir.cFileName[0] == L'.') { continue; }
        std::wstring Path = Projects + L"\\" + Dir.cFileName + L"\\" + Want;
        if(GetFileAttributesW(Path.c_str()) != INVALID_FILE_ATTRIBUTES) { Found = Path; }
    } while(Found.empty() && FindNextFileW(DirFind, &Dir));
    FindClose(DirFind);
    return Found;
}

// A claude's transcript as read so far (MSG_HISTORY_GET): what the user typed, what claude wrote and each tool it used
// (with its result), oldest first; 64 KB a message at most. Each read takes only what was written since (a turn under
// way is read again and again), the first one the last 16 MB.
struct history_entry { u32 Kind; std::string Time, Text; };   // Kind: 0 claude, 1 the user, 2 a tool used
struct history_cache
{
    std::string SessionId;
    std::wstring Path;        // its transcript ("" while there's none)
    u64 Offset;               // read up to here (whole lines)
    u64 Seen;                 // the file's size at the last read
    std::vector<history_entry> Entries;
    std::map<std::string, size_t> Pending;   // the tools used whose result hasn't come yet: their entry
    std::set<std::string> ToolIds;           // the tools used so far (claude writes some twice)
    u32 Version;              // the last change's (counted server-wide, so never the same twice)
    u32 ResetVersion;         // when it last started over (another session, a shorter file)
    u64 UsedMs;               // asked for last (unused ones go)
};

static volatile LONG g_HistoryVersion;

// What was written to SessionId's transcript since the last read; another session or a shorter file starts over.
static void ReadHistoryMore(history_cache& C, const std::wstring& ConfigDir, const std::string& SessionId)
{
    // versions start from the clock, so a window's from before a server restart isn't taken for one of this server's
    if(!g_HistoryVersion) { InterlockedCompareExchange(&g_HistoryVersion, (LONG)(GetTickCount() & 0x0FFFFFFF) + 1, 0); }
    bool32 Reset = SessionId != C.SessionId;
    if(Reset || C.Path.empty())
    {
        std::wstring Path = SessionFilePath(ConfigDir, SessionId);
        Reset |= Path != C.Path;
        C.Path = Path;
    }
    HANDLE File = C.Path.empty() ? INVALID_HANDLE_VALUE :
                  CreateFileW(C.Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    LARGE_INTEGER Size = {};
    if(File != INVALID_HANDLE_VALUE) { GetFileSizeEx(File, &Size); }
    if((u64)Size.QuadPart < C.Offset || !C.Version) { Reset = true; }
    if(Reset)
    {
        C.SessionId = SessionId;
        C.Offset = C.Seen = 0;
        C.Entries.clear();
        C.Pending.clear();
        C.ToolIds.clear();
        C.Version = C.ResetVersion = (u32)InterlockedIncrement(&g_HistoryVersion);
    }
    bool32 Changed = false;
    if(File != INVALID_HANDLE_VALUE)
    {
        const u64 Most = 16ull * 1024 * 1024;
        bool32 First = !C.Offset;   // the first read may start in the middle of a line
        u64 Start = C.Offset ? C.Offset : (u64)Size.QuadPart > Most ? (u64)Size.QuadPart - Most : 0;
        std::string Buf = ReadFileRange(File, Start, (u32)((u64)Size.QuadPart - Start));
        CloseHandle(File);
        C.Seen = (u64)Size.QuadPart;
        // whole lines only: a line being written is read next time
        size_t Whole = Buf.rfind('\n');
        Whole = Whole == std::string::npos ? 0 : Whole + 1;
        C.Offset = Start + Whole;
        size_t LineStart = 0;
        if(First && Start > 0) { LineStart = Buf.find('\n') + 1; } // the first line is cut
        while(LineStart < Whole)
        {
            size_t LineEnd = Buf.find('\n', LineStart);
            std::string Line = Buf.substr(LineStart, LineEnd - LineStart);
            LineStart = LineEnd + 1;
            if(Line.find("\"isSidechain\":true") != std::string::npos || Line.find("\"isMeta\":true") != std::string::npos) { continue; }
            if(!Line.compare(0, 20, "{\"type\":\"api-request")) { continue; }   // what was sent to the API: the conversation again
            history_entry E = {};
            if(Line.find("\"role\":\"user\",\"content\":\"") != std::string::npos)
            {
                // a prompt as typed (tool results are arrays of blocks, not a string)
                E.Kind = 1;
                size_t At = Line.find("\"role\":\"user\",\"content\":\"");
                JsonDecodeString(Line, At + 25, E.Text);
                if(!E.Text.empty() && E.Text[0] == '<') { continue; } // a command's or hook's wrapper, not words
            }
            else if(Line.find("\"role\":\"assistant\"") != std::string::npos)
            {
                // claude's text blocks, in order; then each tool it used, an entry of its own (its result comes later)
                const char* Pattern = "\"type\":\"text\",\"text\":\"";
                for(size_t At = Line.find(Pattern); At != std::string::npos; At = Line.find(Pattern, At + 1))
                {
                    std::string Part;
                    JsonDecodeString(Line, At + strlen(Pattern), Part);
                    if(Part.empty()) { continue; }
                    if(!E.Text.empty()) { E.Text += "\n\n"; }
                    E.Text += Part;
                }
                for(size_t At = Line.find("\"type\":\"tool_use\""); At != std::string::npos; At = Line.find("\"type\":\"tool_use\"", At + 1))
                {
                    // --- its input object (braces counted outside strings) and the fields that say what it did ---
                    std::string Id, Name;
                    size_t Input = Line.find("\"input\":{", At);
                    if(Input == std::string::npos) { continue; }
                    std::string Head = Line.substr(At, Input - At);
                    JsonFindString(Head, "id", false, Id);
                    JsonFindString(Head, "name", false, Name);
                    if(!Id.empty() && !C.ToolIds.insert(Id).second) { continue; }   // written twice
                    size_t End = Input + 8;
                    s32 Depth = 0;
                    bool32 InString = false;
                    for(; End < Line.size(); ++End)
                    {
                        char Ch = Line[End];
                        if(InString) { if(Ch == '\\') { End++; } else if(Ch == '"') { InString = false; } continue; }
                        if(Ch == '"') { InString = true; }
                        else if(Ch == '{') { Depth++; }
                        else if(Ch == '}' && --Depth == 0) { End++; break; }
                    }
                    std::string In = Line.substr(Input + 8, End - Input - 8);
                    std::string Summary, Detail, A, B;
                    if(JsonFindString(In, "command", false, A)) { Summary = A.substr(0, A.find('\n')); Detail = A; }
                    else if(JsonFindString(In, "file_path", false, A) || JsonFindString(In, "notebook_path", false, A))
                    {
                        Summary = A;
                        std::string Old, New;
                        if(JsonFindString(In, "old_string", false, Old) | JsonFindString(In, "new_string", false, New))
                        {
                            // an edit as a diff: the old lines with '-', the new with '+'
                            for(size_t P = 0; P <= Old.size() && !Old.empty();) { size_t Q = Old.find('\n', P); if(Q == std::string::npos) { Q = Old.size(); } Detail += "-" + Old.substr(P, Q - P) + "\n"; P = Q + 1; }
                            for(size_t P = 0; P <= New.size() && !New.empty();) { size_t Q = New.find('\n', P); if(Q == std::string::npos) { Q = New.size(); } Detail += "+" + New.substr(P, Q - P) + "\n"; P = Q + 1; }
                        }
                        else if(JsonFindString(In, "content", false, B)) { Detail = B; }
                    }
                    else if(JsonFindString(In, "pattern", false, A)) { Summary = A; }
                    else if(JsonFindString(In, "url", false, A) || JsonFindString(In, "query", false, A) || JsonFindString(In, "description", false, A) ||
                            JsonFindString(In, "prompt", false, A)) { Summary = A.substr(0, A.find('\n')); }
                    if(Detail.empty()) { Detail = In.size() > 4096 ? In.substr(0, 4096) : In; }
                    if(Detail.size() > 32 * 1024) { Detail.resize(32 * 1024); }
                    history_entry T = {};
                    T.Kind = 2;
                    T.Text = Name + "\x1f" + Summary + "\x1f" + Detail + "\x1f";
                    JsonFindString(Line, "timestamp", true, T.Time);
                    if(!E.Text.empty())
                    {
                        // the text before it first
                        if(E.Text.size() > 64 * 1024) { E.Text.resize(64 * 1024); }
                        E.Time = T.Time;
                        if(!C.Entries.empty() && C.Entries.back().Kind == 0 && C.Entries.back().Time == E.Time) { C.Entries.back().Text += "\n\n" + E.Text; }
                        else { C.Entries.push_back(E); }
                        Changed = true;
                        E.Text.clear();
                    }
                    C.Pending[Id] = C.Entries.size();
                    C.Entries.push_back(T);
                    Changed = true;
                }
            }
            else if(Line.find("\"type\":\"tool_result\"") != std::string::npos)
            {
                // --- results: each goes on the end of its tool's entry (its text, or its first text block) ---
                for(size_t At = Line.find("\"tool_use_id\":\""); At != std::string::npos; At = Line.find("\"tool_use_id\":\"", At + 1))
                {
                    std::string Id, Out;
                    JsonDecodeString(Line, At + 15, Id);
                    auto P = C.Pending.find(Id);
                    if(P == C.Pending.end() || P->second >= C.Entries.size()) { continue; }
                    size_t Content = Line.find("\"content\":", At);
                    if(Content == std::string::npos) { continue; }
                    if(Content + 10 < Line.size() && Line[Content + 10] == '"') { JsonDecodeString(Line, Content + 11, Out); }
                    else { size_t T = Line.find("\"text\":\"", Content); if(T != std::string::npos) { JsonDecodeString(Line, T + 8, Out); } }
                    if(Out.size() > 16 * 1024) { Out.resize(16 * 1024); }
                    C.Entries[P->second].Text += Out;
                    Changed = true;
                    C.Pending.erase(P);
                }
                continue;
            }
            if(E.Text.empty()) { continue; }
            if(E.Text.size() > 64 * 1024) { E.Text.resize(64 * 1024); }
            JsonFindString(Line, "timestamp", true, E.Time);
            // a message written in parts (one line per block) goes on in the same entry
            if(E.Kind == 0 && !C.Entries.empty() && C.Entries.back().Kind == 0 && C.Entries.back().Time == E.Time)
            {
                C.Entries.back().Text += "\n\n" + E.Text;
                Changed = true;
                continue;
            }
            C.Entries.push_back(E);
            Changed = true;
        }
    }
    if(Changed) { C.Version = (u32)InterlockedIncrement(&g_HistoryVersion); }
}
