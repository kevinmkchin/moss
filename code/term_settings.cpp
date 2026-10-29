// The user's settings file, %USERPROFILE%\.moss\settings.txt: "key = value" lines, '#' comments.
// Read by the server and the window (font, font_size, opacity, ...). A missing file or key = the default.

static std::wstring SettingsPath()
{
    wchar_t Home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
    return std::wstring(Home) + L"\\.moss\\settings.txt";
}

// The value for Key, trimmed; empty when absent.
static std::string ReadSetting(const char* Key)
{
    std::string Value;
    FILE* File = 0;
    _wfopen_s(&File, SettingsPath().c_str(), L"rb");
    char Line[512];
    while(File && fgets(Line, sizeof(Line), File))
    {
        std::string S = Line;
        size_t Eq = S.find('=');
        if(S[0] == '#' || Eq == std::string::npos) { continue; }
        std::string K = S.substr(0, Eq);
        while(!K.empty() && strchr(" \t", K.back())) { K.pop_back(); }
        while(!K.empty() && strchr(" \t", K[0])) { K.erase(0, 1); }
        if(K != Key) { continue; }
        Value = S.substr(Eq + 1);
        while(!Value.empty() && strchr(" \t\r\n", Value.back())) { Value.pop_back(); }
        while(!Value.empty() && strchr(" \t", Value[0])) { Value.erase(0, 1); }
    }
    if(File) { fclose(File); }
    return Value;
}

// Sets Key to Value in settings.txt, keeping every other line (the window's zoom, theme, opacity).
static void WriteSetting(const char* Key, const std::string& Value)
{
    std::string Kept;
    FILE* File = 0;
    _wfopen_s(&File, SettingsPath().c_str(), L"rb");
    char Line[512];
    while(File && fgets(Line, sizeof(Line), File))
    {
        std::string K = Line;
        K = K.substr(0, K.find('='));
        while(!K.empty() && strchr(" \t\r\n", K.back())) { K.pop_back(); }
        while(!K.empty() && strchr(" \t", K[0])) { K.erase(0, 1); }
        if(K != Key) { Kept += Line; }
    }
    if(File) { fclose(File); }
    if(!Kept.empty() && Kept.back() != '\n') { Kept += "\r\n"; }
    Kept += std::string(Key) + " = " + Value + "\r\n";
    std::wstring Path = SettingsPath();
    CreateDirectoryW(Path.substr(0, Path.find_last_of(L'\\')).c_str(), 0);
    _wfopen_s(&File, Path.c_str(), L"wb");
    if(File) { fwrite(Kept.data(), 1, Kept.size(), File); fclose(File); }
}

// A whole file, or empty when it can't be read (the server's caches).
static std::string ReadWholeFile(const std::wstring& Path)
{
    std::string Out;
    FILE* File = 0;
    _wfopen_s(&File, Path.c_str(), L"rb");
    char Buf[4096];
    for(size_t N = 0; File && (N = fread(Buf, 1, sizeof(Buf), File)) > 0;) { Out.append(Buf, N); }
    if(File) { fclose(File); }
    return Out;
}

// The folder moss.exe really is in, with a trailing backslash: clink, OpenConsole and the default shell setup ship there.
// Followed through links: winget starts a portable app through a symbolic link in its Links folder.
static std::wstring ExeDir()
{
    wchar_t Self[MAX_PATH] = {}, Real[MAX_PATH] = {};
    GetModuleFileNameW(0, Self, MAX_PATH);
    std::wstring Dir = Self;
    HANDLE File = CreateFileW(Self, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if(File != INVALID_HANDLE_VALUE)
    {
        DWORD Len = GetFinalPathNameByHandleW(File, Real, MAX_PATH, FILE_NAME_NORMALIZED);
        CloseHandle(File);
        if(Len > 0 && Len < MAX_PATH)
        {
            Dir = Real;
            if(Dir.rfind(L"\\\\?\\UNC\\", 0) == 0) { Dir = L"\\\\" + Dir.substr(8); }
            else if(Dir.rfind(L"\\\\?\\", 0) == 0) { Dir = Dir.substr(4); }
        }
    }
    return Dir.substr(0, Dir.find_last_of(L'\\') + 1);
}

// %USERPROFILE%\.moss\, with a trailing backslash: the user's own shell setup and clink profile.
static std::wstring UserTermDir()
{
    wchar_t Home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
    return std::wstring(Home) + L"\\.moss\\";
}

// Copies the folder From into To (made as needed), every file over what's there; "logs" folders are left out.
static void CopyTree(const std::wstring& From, const std::wstring& To)
{
    CreateDirectoryW(To.c_str(), 0);
    WIN32_FIND_DATAW Found = {};
    HANDLE Find = FindFirstFileW((From + L"\\*").c_str(), &Found);
    if(Find == INVALID_HANDLE_VALUE) { return; }
    do
    {
        std::wstring Name = Found.cFileName;
        if(Name == L"." || Name == L".." || Name == L"logs") { continue; }
        if(Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { CopyTree(From + L"\\" + Name, To + L"\\" + Name); }
        else { CopyFileW((From + L"\\" + Name).c_str(), (To + L"\\" + Name).c_str(), FALSE); }
    } while(FindNextFileW(Find, &Found));
    FindClose(Find);
}

// moss was called term while it was made. Its users' things move over once: ~\.term to ~\.moss and %LOCALAPPDATA%\term
// to %LOCALAPPDATA%\moss (renamed; the default clink script becomes moss.lua with moss's `view`), and "start with
// Windows" / "Open in term" become moss's (this exe). While a term server still runs, they're copied instead and a
// marker (~\.moss\.from-term) says so: each start copies them again until term is gone, then they move for good.
static void MigrateFromTerm()
{
    wchar_t Home[MAX_PATH] = {}, Local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
    GetEnvironmentVariableW(L"LOCALAPPDATA", Local, MAX_PATH);
    std::wstring OldHome = std::wstring(Home) + L"\\.term", NewHome = std::wstring(Home) + L"\\.moss";
    std::wstring OldLocal = std::wstring(Local) + L"\\term", NewLocal = std::wstring(Local) + L"\\moss";
    std::wstring Marker = NewHome + L"\\.from-term";
    bool32 OldThere = GetFileAttributesW(OldHome.c_str()) != INVALID_FILE_ATTRIBUTES || GetFileAttributesW(OldLocal.c_str()) != INVALID_FILE_ATTRIBUTES;
    bool32 NewThere = GetFileAttributesW(NewHome.c_str()) != INVALID_FILE_ATTRIBUTES;
    bool32 Copied = GetFileAttributesW(Marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    if(!OldThere || (NewThere && !Copied)) { return; }
    // a term server still running: its pipe is there
    std::wstring OldPipe = L"\\\\.\\pipe\\term-" + CurrentUserSid();
    bool32 TermRuns = WaitNamedPipeW(OldPipe.c_str(), 1) || GetLastError() != ERROR_FILE_NOT_FOUND;
    if(TermRuns)
    {
        CopyTree(OldHome, NewHome);
        CopyTree(OldLocal, NewLocal);
        HANDLE M = CreateFileW(Marker.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, 0);
        if(M != INVALID_HANDLE_VALUE) { CloseHandle(M); }
        Logf("settings", "moved from term: copied (term still runs)");
    }
    else
    {
        // for good: what moss copied before goes, the old folders become moss's (copied when something holds them)
        if(Copied) { DeleteFileW(Marker.c_str()); }
        if(!MoveFileExW(OldHome.c_str(), NewHome.c_str(), 0)) { CopyTree(OldHome, NewHome); }
        if(!MoveFileExW(OldLocal.c_str(), NewLocal.c_str(), 0)) { CopyTree(OldLocal, NewLocal); }
        // "start with Windows" and "Open in term" become moss's
        wchar_t Exe[MAX_PATH] = {};
        GetModuleFileNameW(0, Exe, MAX_PATH);
        HKEY Run = 0;
        if(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ | KEY_WRITE, &Run) == ERROR_SUCCESS)
        {
            if(RegQueryValueExW(Run, L"term", 0, 0, 0, 0) == ERROR_SUCCESS)
            {
                std::wstring Command = std::wstring(L"\"") + Exe + L"\" --server";
                RegSetValueExW(Run, L"moss", 0, REG_SZ, (const BYTE*)Command.c_str(), (DWORD)((Command.size() + 1) * 2));
                RegDeleteValueW(Run, L"term");
            }
            RegCloseKey(Run);
        }
        static const wchar_t* const Menus[2] = {L"Software\\Classes\\Directory\\shell\\", L"Software\\Classes\\Directory\\Background\\shell\\"};
        for(s32 K = 0; K < 2; ++K)
        {
            HKEY Old = 0;
            if(RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(Menus[K]) + L"term").c_str(), 0, KEY_READ, &Old) != ERROR_SUCCESS) { continue; }
            RegCloseKey(Old);
            RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(Menus[K]) + L"term").c_str());
            HKEY Key = 0, Sub = 0;
            if(RegCreateKeyExW(HKEY_CURRENT_USER, (std::wstring(Menus[K]) + L"moss").c_str(), 0, 0, 0, KEY_WRITE, 0, &Key, 0) != ERROR_SUCCESS) { continue; }
            std::wstring Label = L"Open in moss", Command = std::wstring(L"\"") + Exe + L"\" -d \"%" + (K ? L"V" : L"1") + L"\"";
            RegSetValueExW(Key, 0, 0, REG_SZ, (const BYTE*)Label.c_str(), (DWORD)((Label.size() + 1) * 2));
            RegSetValueExW(Key, L"Icon", 0, REG_SZ, (const BYTE*)Exe, (DWORD)((wcslen(Exe) + 1) * 2));
            if(RegCreateKeyExW(Key, L"command", 0, 0, 0, KEY_WRITE, 0, &Sub, 0) == ERROR_SUCCESS)
            {
                RegSetValueExW(Sub, 0, 0, REG_SZ, (const BYTE*)Command.c_str(), (DWORD)((Command.size() + 1) * 2));
                RegCloseKey(Sub);
            }
            RegCloseKey(Key);
        }
        Logf("settings", "moved from term: for good");
    }
    // --- the default clink script: moss.lua, its `view` moss's (the user's own edits stay) ---
    std::wstring OldLua = NewHome + L"\\clink\\clod.lua", NewLua = NewHome + L"\\clink\\moss.lua";
    HANDLE In = CreateFileW(OldLua.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if(In == INVALID_HANDLE_VALUE) { return; }
    std::string Text;
    char Buf[4096];
    DWORD Got = 0;
    while(ReadFile(In, Buf, sizeof(Buf), &Got, 0) && Got) { Text.append(Buf, Got); }
    CloseHandle(In);
    static const char* const Swaps[][2] = {
        {"os.getenv(\"CLOD_DIR\")", "os.getenv(\"MOSS_DIR\")"}, {"os.getenv(\"TERM_DIR\")", "os.getenv(\"MOSS_DIR\")"},
        {"'clod.exe\" view $*'", "'moss.exe\" view $*'"}, {"'term.exe\" view $*'", "'moss.exe\" view $*'"},
    };
    for(const auto& Swap : Swaps)
    {
        for(size_t At = Text.find(Swap[0]); At != std::string::npos; At = Text.find(Swap[0], At + strlen(Swap[1]))) { Text.replace(At, strlen(Swap[0]), Swap[1]); }
    }
    HANDLE Out = CreateFileW(NewLua.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if(Out == INVALID_HANDLE_VALUE) { return; }
    DWORD Wrote = 0;
    WriteFile(Out, Text.data(), (DWORD)Text.size(), &Wrote, 0);
    CloseHandle(Out);
    DeleteFileW(OldLua.c_str());
}

// First run (no ~\.moss\init.bat): gives the user their own copy of term's defaults (shell setup,
// settings.txt). Run by the server at start and by the window before it reads settings.txt.
// Only missing files are copied, so a user's edits are never overwritten; from then on ~\.moss
// is theirs. Deleting init.bat brings back any default they removed.
static void SeedUserSetup()
{
    MigrateFromTerm();
    std::wstring Defaults = ExeDir() + L"shell\\";
    std::wstring ClodDir = UserTermDir();
    if(GetFileAttributesW((ClodDir + L"init.bat").c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::vector<std::wstring> Dirs = {std::wstring()}; // relative to both roots
        for(size_t K = 0; K < Dirs.size(); ++K)
        {
            CreateDirectoryW((ClodDir + Dirs[K]).c_str(), 0);
            WIN32_FIND_DATAW Found = {};
            HANDLE Find = FindFirstFileW((Defaults + Dirs[K] + L"*").c_str(), &Found);
            if(Find == INVALID_HANDLE_VALUE) { continue; }
            do
            {
                std::wstring Name = Found.cFileName;
                if(Name == L"." || Name == L"..") { continue; }
                if(Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { Dirs.push_back(Dirs[K] + Name + L"\\"); }
                else { CopyFileW((Defaults + Dirs[K] + Name).c_str(), (ClodDir + Dirs[K] + Name).c_str(), TRUE); }
            } while(FindNextFileW(Find, &Found));
            FindClose(Find);
        }
    }
}

// "Reset to moss's defaults": settings.txt becomes the default one moss ships (none: no settings.txt), the old one
// kept as settings-old.txt.
static void ResetSettings()
{
    std::wstring Path = SettingsPath();
    std::wstring Old = Path.substr(0, Path.size() - 4) + L"-old.txt";
    MoveFileExW(Path.c_str(), Old.c_str(), MOVEFILE_REPLACE_EXISTING);
    CopyFileW((ExeDir() + L"shell\\settings.txt").c_str(), Path.c_str(), FALSE);
}

// "Be a good programmer" lives in the global CLAUDE.md, which every Claude Code session on the machine reads (in
// moss or not): on = the user's principles (~\.moss\good-programmer.md) sit there between these markers, off = they
// don't. The rest of the file is the user's and is kept as it is. clod's block (the same thing, its own markers)
// counts as on, and off takes it out too.
static const char* GoodMarks[2][2] = {
    {"<!-- moss: Be a good programmer (start). moss's settings add and remove this block. -->", "<!-- moss: Be a good programmer (end) -->"},
    {"<!-- clod: Be a good programmer (start).", "<!-- clod: Be a good programmer (end) -->"},
};

static bool32 GoodProgrammerOn()
{
    std::string Md = ReadWholeFile(ClaudeConfigDir() + L"\\CLAUDE.md");
    return Md.find(GoodMarks[0][0]) != std::string::npos || Md.find(GoodMarks[1][0]) != std::string::npos;
}

// Puts the principles block into the global CLAUDE.md (On) or takes it out. False when the file couldn't be written
// or there are no principles to add.
static bool32 SetGoodProgrammer(bool32 On)
{
    std::wstring Path = ClaudeConfigDir() + L"\\CLAUDE.md";
    std::string Md = ReadWholeFile(Path);

    // --- drop the blocks (and the blank lines before them), wherever they are ---
    for(const char* const* Mark : GoodMarks)
    {
        size_t Start = Md.find(Mark[0]);
        size_t End = Start == std::string::npos ? std::string::npos : Md.find(Mark[1], Start);
        if(End == std::string::npos) { continue; }
        End += strlen(Mark[1]);
        while(End < Md.size() && (Md[End] == '\r' || Md[End] == '\n')) { ++End; }
        while(Start > 0 && (Md[Start - 1] == '\r' || Md[Start - 1] == '\n')) { --Start; }
        Md.erase(Start, End - Start);
        if(!Md.empty()) { Md += "\r\n"; }
    }

    // --- add it at the end, from the user's copy (else moss's default) ---
    if(On)
    {
        std::string Principles = ReadWholeFile(UserTermDir() + L"good-programmer.md");
        if(Principles.empty()) { Principles = ReadWholeFile(ExeDir() + L"shell\\good-programmer.md"); }
        if(Principles.empty()) { return false; }
        while(!Principles.empty() && strchr("\r\n", Principles.back())) { Principles.pop_back(); }
        Md += (Md.empty() ? "" : "\r\n") + std::string(GoodMarks[0][0]) + "\r\n" + Principles + "\r\n" + GoodMarks[0][1] + "\r\n";
    }

    // --- written beside it, then swapped in, so a session reading it never sees half a file ---
    CreateDirectoryW(ClaudeConfigDir().c_str(), 0);
    std::wstring Temp = Path + L".moss-tmp";
    FILE* File = 0;
    _wfopen_s(&File, Temp.c_str(), L"wb");
    if(!File) { return false; }
    bool32 Ok = fwrite(Md.data(), 1, Md.size(), File) == Md.size();
    fclose(File);
    return Ok && MoveFileExW(Temp.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING);
}


