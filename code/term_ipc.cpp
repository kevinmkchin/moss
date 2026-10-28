// Client/server transport: framed messages over a per-user named pipe.
// Frame = u32 Type, u32 Size, Size bytes of payload. Byte-mode pipe, overlapped IO on both ends.

#include "term_ipc_proto.h"

struct ipc_conn
{
    HANDLE Pipe;
    HANDLE ReadEvent;
    HANDLE WriteEvent;
    OVERLAPPED ReadOv;
    bool32 ReadPending;
    std::string Inbox; // received bytes not yet split into messages
    char ReadBuf[64 * 1024];
};

static std::wstring CurrentUserSid()
{
    std::wstring Result;
    HANDLE Token = 0;
    if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token)) { return Result; }
    u8 Buf[256] = {};
    DWORD Size = 0;
    if(GetTokenInformation(Token, TokenUser, Buf, sizeof(Buf), &Size))
    {
        wchar_t* Sid = 0;
        if(ConvertSidToStringSidW(((TOKEN_USER*)Buf)->User.Sid, &Sid))
        {
            Result = Sid;
            LocalFree(Sid);
        }
    }
    CloseHandle(Token);
    return Result;
}

// MOSS_INSTANCE=<name> runs a separate server (own pipe, own tabs file), e.g. for tests.
static std::wstring InstanceSuffix()
{
    wchar_t Name[64] = {};
    if(GetEnvironmentVariableW(L"MOSS_INSTANCE", Name, ArrayCount(Name))) { return std::wstring(L"-") + Name; }
    return std::wstring();
}

// Microseconds on the system-wide performance counter (comparable across processes).
static u64 QpcUs()
{
    LARGE_INTEGER Now = {}, Freq = {};
    QueryPerformanceCounter(&Now);
    QueryPerformanceFrequency(&Freq);
    return (u64)((Now.QuadPart / Freq.QuadPart) * 1000000 + (Now.QuadPart % Freq.QuadPart) * 1000000 / Freq.QuadPart);
}

// For a process on the typing path (the window, the server, a tab's console host, cmd, claude): above-normal
// priority so a busy machine (a build) can't hold up an echo, and no power throttling (Windows parks
// windowless processes on slow cores). Children they start still get normal priority.
static void RaiseForTyping(HANDLE Process)
{
    SetPriorityClass(Process, ABOVE_NORMAL_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE Throttle = {};
    Throttle.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    Throttle.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetProcessInformation(Process, ProcessPowerThrottling, &Throttle, sizeof(Throttle));
}

// MOSS_LATENCY=1: each process appends "<what> <microseconds>" lines to %TEMP%\moss-latency-<pid>.txt
// at every hop a keystroke takes (one clock for all processes), for tools\latency.py.
static void LatencyMark(const char* What)
{
    static FILE* File = []() -> FILE*
    {
        if(!GetEnvironmentVariableW(L"MOSS_LATENCY", 0, 0)) { return 0; }
        wchar_t Temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, Temp);
        FILE* F = 0;
        _wfopen_s(&F, (std::wstring(Temp) + L"moss-latency-" + std::to_wstring(GetCurrentProcessId()) + L".txt").c_str(), L"ab");
        return F;
    }();
    if(!File) { return; }
    fprintf(File, "%s %llu\n", What, QpcUs());
    fflush(File);
}

static std::wstring PipeName()
{
    return L"\\\\.\\pipe\\moss-" + CurrentUserSid() + InstanceSuffix();
}

// Server side: a new listening pipe instance that only the current user can open.
static HANDLE IpcCreatePipe(bool32 First)
{
    // Default pipe ACLs grant Everyone read access, which would expose terminal contents.
    std::wstring Sddl = L"D:P(A;;GA;;;" + CurrentUserSid() + L")";
    PSECURITY_DESCRIPTOR Sd = 0;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl.c_str(), SDDL_REVISION_1, &Sd, 0);
    SECURITY_ATTRIBUTES Sa = {};
    Sa.nLength = sizeof(Sa);
    Sa.lpSecurityDescriptor = Sd;
    DWORD Open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (First ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    HANDLE Pipe = CreateNamedPipeW(PipeName().c_str(), Open, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   PIPE_UNLIMITED_INSTANCES, 256 * 1024, 64 * 1024, 0, Sd ? &Sa : 0);
    if(Sd) { LocalFree(Sd); }
    return Pipe;
}

static void IpcInit(ipc_conn& C, HANDLE Pipe)
{
    C.Pipe = Pipe;
    C.ReadEvent = CreateEventW(0, TRUE, FALSE, 0);
    C.WriteEvent = CreateEventW(0, TRUE, FALSE, 0);
    C.ReadPending = false;
    C.Inbox.clear();
}

static void IpcClose(ipc_conn& C)
{
    if(C.Pipe)
    {
        CancelIo(C.Pipe);
        if(C.ReadPending)
        {
            DWORD Ignored = 0;
            GetOverlappedResult(C.Pipe, &C.ReadOv, &Ignored, TRUE);
        }
        CloseHandle(C.Pipe);
    }
    if(C.ReadEvent) { CloseHandle(C.ReadEvent); }
    if(C.WriteEvent) { CloseHandle(C.WriteEvent); }
    C.Pipe = 0;
    C.ReadEvent = 0;
    C.WriteEvent = 0;
    C.ReadPending = false;
    C.Inbox.clear();
}

static bool32 IpcSend(ipc_conn& C, u32 Type, const void* Data, u32 Size)
{
    if(!C.Pipe) { return false; }
    std::string Frame;
    Frame.resize(8 + Size);
    memcpy(&Frame[0], &Type, 4);
    memcpy(&Frame[4], &Size, 4);
    if(Size) { memcpy(&Frame[8], Data, Size); }

    size_t At = 0;
    while(At < Frame.size())
    {
        OVERLAPPED Ov = {};
        Ov.hEvent = C.WriteEvent;
        ResetEvent(C.WriteEvent);
        DWORD Written = 0;
        if(!WriteFile(C.Pipe, Frame.data() + At, (DWORD)(Frame.size() - At), 0, &Ov) && GetLastError() != ERROR_IO_PENDING) { return false; }
        if(!GetOverlappedResult(C.Pipe, &Ov, &Written, TRUE) || Written == 0) { return false; }
        At += Written;
    }
    return true;
}

static bool32 IpcSendString(ipc_conn& C, u32 Type, const std::string& S)
{
    return IpcSend(C, Type, S.data(), (u32)S.size());
}

// Keeps one overlapped read outstanding. Call after IpcInit and whenever ReadEvent fires.
// Returns false once the other end has gone away.
static bool32 IpcPump(ipc_conn& C)
{
    if(!C.Pipe) { return false; }
    if(C.ReadPending)
    {
        if(WaitForSingleObject(C.ReadEvent, 0) != WAIT_OBJECT_0) { return true; }
        DWORD Read = 0;
        C.ReadPending = false;
        if(!GetOverlappedResult(C.Pipe, &C.ReadOv, &Read, FALSE)) { return false; }
        C.Inbox.append(C.ReadBuf, Read);
    }
    for(;;)
    {
        C.ReadOv = OVERLAPPED{};
        C.ReadOv.hEvent = C.ReadEvent;
        ResetEvent(C.ReadEvent);
        DWORD Read = 0;
        if(ReadFile(C.Pipe, C.ReadBuf, sizeof(C.ReadBuf), &Read, &C.ReadOv))
        {
            // Completed synchronously; collect it and keep going.
            GetOverlappedResult(C.Pipe, &C.ReadOv, &Read, FALSE);
            C.Inbox.append(C.ReadBuf, Read);
            continue;
        }
        if(GetLastError() != ERROR_IO_PENDING) { return false; }
        C.ReadPending = true;
        return true;
    }
}

static bool32 IpcNextMessage(ipc_conn& C, u32& Type, std::string& Payload)
{
    if(C.Inbox.size() < 8) { return false; }
    u32 Size = 0;
    memcpy(&Type, C.Inbox.data(), 4);
    memcpy(&Size, C.Inbox.data() + 4, 4);
    if(C.Inbox.size() < 8 + (size_t)Size) { return false; }
    Payload.assign(C.Inbox, 8, Size);
    C.Inbox.erase(0, 8 + (size_t)Size);
    return true;
}

// Connects to the running server, starting one (ServerExe --server) if there is none. Returns null
// on failure. 
static HANDLE ConnectToServer(const std::wstring& ServerExe, std::string& Error)
{
    std::wstring Name = PipeName();
    bool32 Spawned = false;
    u64 Deadline = GetTickCount64() + 5000;
    for(;;)
    {
        HANDLE Pipe = CreateFileW(Name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
        if(Pipe != INVALID_HANDLE_VALUE) { return Pipe; }
        DWORD Err = GetLastError();
        if(Err == ERROR_PIPE_BUSY)
        {
            WaitNamedPipeW(Name.c_str(), 500);
        }
        else if(!Spawned)
        {
            // --- start the background server, outside this console and outside any job that would kill it ---
            std::wstring Cmd = L"\"" + ServerExe + L"\" --server";
            wchar_t Home[MAX_PATH] = {};
            GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
            STARTUPINFOW Si = {};
            Si.cb = sizeof(Si);
            PROCESS_INFORMATION Pi = {};
            DWORD Flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
            BOOL Ok = CreateProcessW(0, &Cmd[0], 0, 0, FALSE, Flags | CREATE_BREAKAWAY_FROM_JOB, 0, Home, &Si, &Pi);
            if(!Ok) { Ok = CreateProcessW(0, &Cmd[0], 0, 0, FALSE, Flags, 0, Home, &Si, &Pi); }
            if(!Ok)
            {
                Error = "could not start the term server (error " + std::to_string(GetLastError()) + ")";
                return 0;
            }
            CloseHandle(Pi.hThread);
            CloseHandle(Pi.hProcess);
            Spawned = true;
        }
        else
        {
            Sleep(30);
        }
        if(GetTickCount64() > Deadline)
        {
            Error = "the term server did not start";
            return 0;
        }
    }
}

// The monitor Window is on, as "<device> <width>x<height>" (e.g. "\\.\DISPLAY1 3840x2160"), so a
// laptop screen and a 4K monitor each remember their own layout. Shared by both kinds of client.
static std::string MonitorKey(HWND Window)
{
    MONITORINFOEXA Info = {};
    Info.cbSize = sizeof(Info);
    if(!Window || !GetMonitorInfoA(MonitorFromWindow(Window, MONITOR_DEFAULTTONEAREST), &Info)) { return std::string(); }
    char Buf[96];
    snprintf(Buf, sizeof(Buf), "%s %dx%d", Info.szDevice, (int)(Info.rcMonitor.right - Info.rcMonitor.left), (int)(Info.rcMonitor.bottom - Info.rcMonitor.top));
    return Buf;
}
