// term: run logs, asserts, crash handler, frame profiler (see term_debug.h).
#include "term_debug.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#pragma comment(lib, "dbghelp.lib")

static struct {
    HANDLE file;
    SRWLOCK lock;
    int echo;
    char mode[16];
    char path[MAX_PATH];         // this run's log
    char dir[MAX_PATH];          // logs folder
    char run[MAX_PATH];          // logs\<run> without extension (dump and symbols folder names)
    volatile uint32_t frame;
    void *exe_base;
    uint64_t exe_size;
    volatile long crashing;

    // profiler
    DebugSite *volatile sites;
    volatile long slot;
    uint32_t closed;             // frames closed so far
    uint64_t work_us[Debug_ProfileFrames];
    uint64_t phase_us[Debug_ProfileFrames][8];
    const char *phase_names[8];
    int phase_count;
    uint64_t tsc_start, qpc_start, qpc_freq;
    double tsc_per_us;
    uint64_t slow_us;
    int slow_run;                // slow frames in a row that logged
    uint64_t last_slow_log_us;
} g_Debug;

int g_DebugPerfApi;
typedef void PerfBeginFn(const char *id, const char *data, uint32_t color);
typedef struct { int64_t suppress[3]; } PerfEndResult;
typedef PerfEndResult PerfEndFn(void);
typedef void PerfNameFn(const char *name);
static struct {
    PerfNameFn *SetCurrentThreadName;
    void *SetCurrentThreadNameN;
    PerfBeginFn *BeginEvent;
    void *BeginEventN, *BeginEventWide, *BeginEventWideN;
    PerfEndFn *EndEvent;
    void *RegisterFiber, *UnregisterFiber, *BeginFiberSwitch, *EndFiberSwitch;
} g_Perf;

static uint64_t NowUs(void)
{
    LARGE_INTEGER t = {0};
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart - (LONGLONG)g_Debug.qpc_start) * 1000000ull / (g_Debug.qpc_freq ? g_Debug.qpc_freq : 1);
}

// --- log ---------------------------------------------------------------------------------------------

static void Debug_WriteRaw(const char *text, int len)
{
    if (len <= 0) return;
    AcquireSRWLockExclusive(&g_Debug.lock);
    DWORD written = 0;
    if (g_Debug.file && g_Debug.file != INVALID_HANDLE_VALUE) WriteFile(g_Debug.file, text, (DWORD)len, &written, NULL);
    if (g_Debug.echo) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out && out != INVALID_HANDLE_VALUE) WriteFile(out, text, (DWORD)len, &written, NULL);
    }
    ReleaseSRWLockExclusive(&g_Debug.lock);
    OutputDebugStringA(text);
}

static void LogV(const char *source, const char *file, int line, const char *fmt, va_list args)
{
    char buf[4096];
    const char *base = file ? strrchr(file, '\\') : NULL;
    base = base ? base + 1 : file;
    int n = file ? snprintf(buf, sizeof(buf), "[%u] %s: %s(%d): ", g_Debug.frame, source, base, line)
                 : snprintf(buf, sizeof(buf), "[%u] %s: ", g_Debug.frame, source);
    if (n < 0) n = 0;
    int m = vsnprintf(buf + n, sizeof(buf) - (size_t)n - 2, fmt, args);
    if (m < 0) m = 0;
    n += m;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    buf[n] = 0;
    Debug_WriteRaw(buf, n);
}

void Debug_Log(const char *source, const char *file, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogV(source, file, line, fmt, args);
    va_end(args);
}

static void LogLine(const char *fmt, ...)   // no prefix (headers, crash sections)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    buf[n] = 0;
    Debug_WriteRaw(buf, n);
}

void Debug_LogHresult(const char *source, const char *file, int line, const char *what, long hr)
{
    char msg[512] = {0};
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)hr, 0, msg, sizeof(msg), NULL);
    while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r' || msg[n - 1] == ' ')) msg[--n] = 0;
    Debug_Log(source, file, line, "%s failed: 0x%08X %s", what, (unsigned)hr, msg);
}

void Debug_SetFrame(uint32_t frame) { g_Debug.frame = frame; }
const char *Debug_LogPath(void) { return g_Debug.path; }

void Debug_AssertFailed(const char *expr, const char *file, int line)
{
    Debug_Log("assert", file, line, "ASSERT FAILED: %s", expr);
}

void Debug_Crash(void)
{
    volatile int never = 0;
    AssertAlways(never);
}

// --- crash handler -----------------------------------------------------------------------------------

static const char *ExceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    case 0xC0000409: return "stack buffer overrun (fast fail)";
    case 0xC0000374: return "heap corruption";
    default: return "exception";
    }
}

static int IsFatal(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
           code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
           code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || code == EXCEPTION_IN_PAGE_ERROR;
}

static void CopyNextTo(const char *src, const char *dir)
{
    const char *base = strrchr(src, '\\');
    base = base ? base + 1 : src;
    char dst[MAX_PATH];
    snprintf(dst, sizeof(dst), "%s\\%s", dir, base);
    if (CopyFileA(src, dst, FALSE)) LogLine("symbols: %s", dst);
}

static void WriteCrash(EXCEPTION_POINTERS *ep)
{
    long me = (long)GetCurrentThreadId();
    long was = InterlockedCompareExchange(&g_Debug.crashing, me, 0);
    if (was == me) return;          // the vectored handler already wrote this one; the filter ends the process
    if (was != 0) Sleep(INFINITE);  // another thread is writing a crash: wait for the process to end, not mid-dump
    HANDLE process = GetCurrentProcess();
    EXCEPTION_RECORD *er = ep->ExceptionRecord;

    // --- minidump first: closest to the crash (dbghelp suspends the other threads while it writes) ---
    char dump_path[MAX_PATH];
    snprintf(dump_path, sizeof(dump_path), "%s.dmp", g_Debug.run);
    HANDLE dump = CreateFileA(dump_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL dumped = FALSE;
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = {0};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
                                             MiniDumpWithUnloadedModules | MiniDumpWithFullMemoryInfo
#if TERM_INTERNAL
                                             | MiniDumpWithPrivateReadWriteMemory   // arenas, heaps, cell grids, stacks
#endif
                                             );
        dumped = MiniDumpWriteDump(process, GetCurrentProcessId(), dump, type, &mei, NULL, NULL);
        CloseHandle(dump);
    }

    // --- what happened ---
    uint64_t addr = (uint64_t)er->ExceptionAddress;
    uint64_t base = (uint64_t)g_Debug.exe_base;
    char where[64] = "";
    if (addr >= base && addr < base + g_Debug.exe_size) snprintf(where, sizeof(where), " (exe+0x%llx)", (unsigned long long)(addr - base));
    LogLine("CRASH: %s 0x%08X at 0x%llx%s, thread %u", ExceptionName(er->ExceptionCode), (unsigned)er->ExceptionCode,
            (unsigned long long)addr, where, GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        LogLine("CRASH: %s of address 0x%llx", er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
                (unsigned long long)er->ExceptionInformation[1]);
    }

    // --- the stack, symbolized from the PDB next to the exe ---
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    char exe_dir[MAX_PATH];
    strcpy_s(exe_dir, sizeof(exe_dir), exe_path);
    char *slash = strrchr(exe_dir, '\\');
    if (slash) *slash = 0;
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    if (SymInitialize(process, exe_dir, TRUE)) {
        CONTEXT ctx = *ep->ContextRecord;
        STACKFRAME64 frame = {0};
        frame.AddrPC.Offset = ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        LogLine("CRASH: stack:");
        for (int i = 0; i < 64; i++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &ctx, NULL, SymFunctionTableAccess64,
                             SymGetModuleBase64, NULL) || !frame.AddrPC.Offset) {
                break;
            }
            char sym_buf[sizeof(SYMBOL_INFO) + 256] = {0};
            SYMBOL_INFO *sym = (SYMBOL_INFO *)sym_buf;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            const char *name = SymFromAddr(process, frame.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
            IMAGEHLP_LINE64 line = {0};
            line.SizeOfStruct = sizeof(line);
            DWORD line_disp = 0;
            char module[MAX_PATH] = "?";
            DWORD64 mod_base = SymGetModuleBase64(process, frame.AddrPC.Offset);
            if (mod_base) {
                GetModuleFileNameA((HMODULE)mod_base, module, MAX_PATH);
                char *m = strrchr(module, '\\');
                if (m) memmove(module, m + 1, strlen(m + 1) + 1);
            }
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_disp, &line)) {
                LogLine("  #%02d %s!%s %s(%lu)", i, module, name, line.FileName, line.LineNumber);
            } else {
                LogLine("  #%02d %s!%s +0x%llx", i, module, name, (unsigned long long)disp);
            }
        }
        SymCleanup(process);
    }

    // --- the exe and PDB as they are now: the next build replaces them, and the dump needs them ---
    char sym_dir[MAX_PATH];
    snprintf(sym_dir, sizeof(sym_dir), "%s_symbols", g_Debug.run);
    CreateDirectoryA(sym_dir, NULL);
    CopyNextTo(exe_path, sym_dir);
    char pdb[MAX_PATH];
    strcpy_s(pdb, sizeof(pdb), exe_path);
    char *dot = strrchr(pdb, '.');
    if (dot) strcpy_s(dot, sizeof(pdb) - (size_t)(dot - pdb), ".pdb");
    CopyNextTo(pdb, sym_dir);
    if (dumped) LogLine("minidump: %s", dump_path);
    else LogLine("minidump: not written (0x%08X)", (unsigned)GetLastError());
    if (g_Debug.file) FlushFileBuffers(g_Debug.file);
}

static LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS *ep)
{
    // Only fatal exceptions in our own code: drivers and runtimes raise and handle their own.
    uint64_t addr = (uint64_t)ep->ExceptionRecord->ExceptionAddress;
    uint64_t base = (uint64_t)g_Debug.exe_base;
    if (IsFatal(ep->ExceptionRecord->ExceptionCode) && addr >= base && addr < base + g_Debug.exe_size) WriteCrash(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI UnhandledHandler(EXCEPTION_POINTERS *ep)
{
    WriteCrash(ep);   // returns at once when the vectored handler already wrote this crash
    return EXCEPTION_EXECUTE_HANDLER;   // end with the exception code, no Windows error dialog
}

// --- init --------------------------------------------------------------------------------------------

static int CompareNames(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void DeleteFolder(const char *dir)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    while (h != INVALID_HANDLE_VALUE) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char f[MAX_PATH];
            snprintf(f, sizeof(f), "%s\\%s", dir, fd.cFileName);
            DeleteFileA(f);
        }
        if (!FindNextFileA(h, &fd)) break;
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    RemoveDirectoryA(dir);
}

void Debug_Init(const char *mode, int echo_stdout)
{
    InitializeSRWLock(&g_Debug.lock);
    LARGE_INTEGER f = {0}, t = {0};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    g_Debug.qpc_freq = (uint64_t)f.QuadPart;
    g_Debug.qpc_start = (uint64_t)t.QuadPart;
    g_Debug.tsc_start = __rdtsc();
    g_Debug.echo = echo_stdout;
    strncpy_s(g_Debug.mode, sizeof(g_Debug.mode), mode, _TRUNCATE);

    // --- our own image's range, for the vectored handler ---
    HMODULE exe = GetModuleHandleA(NULL);
    g_Debug.exe_base = exe;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)exe;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)exe + dos->e_lfanew);
    g_Debug.exe_size = nt->OptionalHeader.SizeOfImage;

    // --- logs folder: next to a dev build's exe, else %LOCALAPPDATA%\moss\logs ---
    char exe_path[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe_path, MAX_PATH);
#if TERM_INTERNAL
    strcpy_s(g_Debug.dir, sizeof(g_Debug.dir), exe_path);
    char *slash = strrchr(g_Debug.dir, '\\');
    if (slash) strcpy_s(slash, sizeof(g_Debug.dir) - (size_t)(slash - g_Debug.dir), "\\logs");
#else
    char local[MAX_PATH] = {0};
    GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
    snprintf(g_Debug.dir, sizeof(g_Debug.dir), "%s\\term", local);
    CreateDirectoryA(g_Debug.dir, NULL);
    strcat_s(g_Debug.dir, sizeof(g_Debug.dir), "\\logs");
#endif
    CreateDirectoryA(g_Debug.dir, NULL);

    // --- the previous run of this mode: did it crash? (newest log of the mode before ours) ---
    static char names[512][64];
    int name_count = 0;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*_%s_*.log", g_Debug.dir, mode);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    while (h != INVALID_HANDLE_VALUE && name_count < 512) {
        strncpy_s(names[name_count++], 64, fd.cFileName, _TRUNCATE);
        if (!FindNextFileA(h, &fd)) break;
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    qsort(names, (size_t)name_count, 64, CompareNames);
    char previous_crash[MAX_PATH] = "";
    if (name_count > 0) {
        char prev[MAX_PATH];
        snprintf(prev, sizeof(prev), "%s\\%s", g_Debug.dir, names[name_count - 1]);
        HANDLE pf = CreateFileA(prev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (pf != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size = {0};
            GetFileSizeEx(pf, &size);
            static char tail[65537];
            LARGE_INTEGER at = {0};
            at.QuadPart = size.QuadPart > 65536 ? size.QuadPart - 65536 : 0;
            SetFilePointerEx(pf, at, NULL, FILE_BEGIN);
            DWORD got = 0;
            ReadFile(pf, tail, 65536, &got, NULL);
            tail[got] = 0;
            if (strstr(tail, "\nCRASH:")) strcpy_s(previous_crash, sizeof(previous_crash), prev);
            CloseHandle(pf);
        }
    }

    // --- keep the newest 4 dumps (ours makes 5) and their symbols folders ---
    {
        static char dumps[256][64];
        int dump_count = 0;
        snprintf(pattern, sizeof(pattern), "%s\\*.dmp", g_Debug.dir);
        h = FindFirstFileA(pattern, &fd);
        while (h != INVALID_HANDLE_VALUE && dump_count < 256) {
            strncpy_s(dumps[dump_count++], 64, fd.cFileName, _TRUNCATE);
            if (!FindNextFileA(h, &fd)) break;
        }
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
        qsort(dumps, (size_t)dump_count, 64, CompareNames);
        for (int i = 0; i + 4 < dump_count; i++) {
            char p[MAX_PATH];
            snprintf(p, sizeof(p), "%s\\%s", g_Debug.dir, dumps[i]);
            DeleteFileA(p);
            char *dot = strrchr(p, '.');
            if (dot) { *dot = 0; strcat_s(p, sizeof(p), "_symbols"); DeleteFolder(p); }
        }
    }

    // --- open this run's log ---
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(g_Debug.run, sizeof(g_Debug.run), "%s\\%04d-%02d-%02d_%02d%02d%02d_%s_%lu", g_Debug.dir, st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond, mode, GetCurrentProcessId());
    snprintf(g_Debug.path, sizeof(g_Debug.path), "%s.log", g_Debug.run);
    g_Debug.file = CreateFileA(g_Debug.path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    // --- header ---
    OSVERSIONINFOEXW os = {0};
    os.dwOSVersionInfoSize = sizeof(os);
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW *);
    RtlGetVersionFn rtl_get_version = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    if (rtl_get_version) rtl_get_version(&os);
    char cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(MAX_PATH, cwd);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    LogLine("moss %s run, pid %lu, started %04d-%02d-%02d %02d:%02d:%02d", mode, GetCurrentProcessId(), st.wYear, st.wMonth,
            st.wDay, st.wHour, st.wMinute, st.wSecond);
    LogLine("log: %s", g_Debug.path);
    LogLine("command line: %s", GetCommandLineA());
    LogLine("working folder: %s", cwd);
    LogLine("exe: %s, built %s %s, %s", exe_path, __DATE__, __TIME__, TERM_INTERNAL ? "internal" : "release");
    LogLine("windows %lu.%lu.%lu, %lu logical processors", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber,
            si.dwNumberOfProcessors);
    if (previous_crash[0]) LogLine("previous %s run crashed: %s", mode, previous_crash);

    // --- crash handlers ---
    AddVectoredExceptionHandler(1, VectoredHandler);
    SetUnhandledExceptionFilter(UnhandledHandler);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    // --- Superluminal, when installed ---
    HMODULE perf = LoadLibraryA("C:\\Program Files\\Superluminal\\Performance\\API\\dll\\x64\\PerformanceAPI.dll");
    if (perf) {
        typedef int GetApiFn(int version, void *out);
        GetApiFn *get_api = (GetApiFn *)(void *)GetProcAddress(perf, "PerformanceAPI_GetAPI");
        if (get_api && get_api((3 << 16) | 0, &g_Perf) && g_Perf.BeginEvent && g_Perf.EndEvent) g_DebugPerfApi = 1;
    }
    g_Debug.slow_us = 8000;
}

// --- profiler ----------------------------------------------------------------------------------------

void Debug_PerfBegin(const char *name) { if (g_Perf.BeginEvent) g_Perf.BeginEvent(name, NULL, 0); }
void Debug_PerfEnd(void) { if (g_Perf.EndEvent) g_Perf.EndEvent(); }
void Debug_NameThread(const char *name) { if (g_Perf.SetCurrentThreadName) g_Perf.SetCurrentThreadName(name); }

void Debug_RecordBlock(DebugSite *site, uint64_t cycles)
{
    if (!site->registered && InterlockedExchange(&site->registered, 1) == 0) {
        DebugSite *head;
        do {
            head = g_Debug.sites;
            site->next = head;
        } while (InterlockedCompareExchangePointer((void *volatile *)&g_Debug.sites, site, head) != head);
    }
    long slot = g_Debug.slot;
    InterlockedAdd64(&site->cycles[slot], (int64_t)cycles);
    InterlockedIncrement(&site->hits[slot]);
}

void Debug_SetPhaseNames(const char *const *names, int count)
{
    g_Debug.phase_count = count < 8 ? count : 8;
    for (int i = 0; i < g_Debug.phase_count; i++) g_Debug.phase_names[i] = names[i];
}

void Debug_SetSlowFrameUs(uint64_t us) { g_Debug.slow_us = us; }

void Debug_RecentWork(int frames, float *avg_ms, float *max_ms)
{
    int n = frames < 1 ? 1 : frames;
    if (n > Debug_ProfileFrames - 1) n = Debug_ProfileFrames - 1;
    if ((uint32_t)n > g_Debug.closed) n = (int)g_Debug.closed;
    long last = (g_Debug.slot + Debug_ProfileFrames - 1) % Debug_ProfileFrames;
    double sum = 0, mx = 0;
    for (int i = 0; i < n; i++) {
        double ms = (double)g_Debug.work_us[(last - i + Debug_ProfileFrames) % Debug_ProfileFrames] / 1000.0;
        sum += ms;
        if (ms > mx) mx = ms;
    }
    *avg_ms = n ? (float)(sum / n) : 0;
    *max_ms = (float)mx;
}

void Debug_FrameEnd(uint64_t work_us, const uint64_t *phase_us)
{
    long slot = g_Debug.slot;
    g_Debug.work_us[slot] = work_us;
    for (int i = 0; i < 8; i++) g_Debug.phase_us[slot][i] = phase_us && i < g_Debug.phase_count ? phase_us[i] : 0;
    g_Debug.closed++;

    // --- TSC rate from the time since init ---
    uint64_t us = NowUs();
    if (us > 0) g_Debug.tsc_per_us = (double)(__rdtsc() - g_Debug.tsc_start) / (double)us;

    // --- open the next slot ---
    long next = (slot + 1) % Debug_ProfileFrames;
    for (DebugSite *s = g_Debug.sites; s; s = s->next) {
        s->cycles[next] = 0;
        s->hits[next] = 0;
    }
    g_Debug.slot = next;

    // --- slow frame: a one-frame profile (up to 5 in a row, then about once a second) ---
    if (g_Debug.slow_us && work_us > g_Debug.slow_us) {
        if (g_Debug.slow_run < 5 || us - g_Debug.last_slow_log_us > 1000000) {
            g_Debug.slow_run++;
            g_Debug.last_slow_log_us = us;
            Debug_LogProfile(1, "slow frame");
        }
    } else {
        g_Debug.slow_run = 0;
    }
}

typedef struct ProfileRow { DebugSite *site; double avg_ms, max_ms, hits; double cycles_per_hit; } ProfileRow;

static int CompareRows(const void *a, const void *b)
{
    double x = ((const ProfileRow *)a)->avg_ms, y = ((const ProfileRow *)b)->avg_ms;
    return x < y ? 1 : x > y ? -1 : 0;
}

void Debug_LogProfile(int frames, const char *why)
{
    int n = frames < 1 ? 1 : frames;
    if (n > Debug_ProfileFrames - 1) n = Debug_ProfileFrames - 1;
    if ((uint32_t)n > g_Debug.closed) n = (int)g_Debug.closed;
    if (n == 0) { Logf("profile", "no frames closed yet"); return; }
    long last = (g_Debug.slot + Debug_ProfileFrames - 1) % Debug_ProfileFrames;
    double per_us = g_Debug.tsc_per_us > 0 ? g_Debug.tsc_per_us : 3000.0;

    double sum = 0, max = 0, phase_sum[8] = {0};
    for (int i = 0; i < n; i++) {
        long s = (last - i + Debug_ProfileFrames) % Debug_ProfileFrames;
        double ms = (double)g_Debug.work_us[s] / 1000.0;
        sum += ms;
        if (ms > max) max = ms;
        for (int p = 0; p < g_Debug.phase_count; p++) phase_sum[p] += (double)g_Debug.phase_us[s][p] / 1000.0;
    }
    Debug_Log("profile", NULL, 0, "%s: last %d frame(s), TSC %.2f GHz: frame work %.3f ms avg, %.3f max", why, n, per_us / 1000.0,
              sum / n, max);
    char phases[512] = "";
    for (int p = 0; p < g_Debug.phase_count; p++) {
        char one[64];
        snprintf(one, sizeof(one), "%s%s %.3f", p ? ", " : "", g_Debug.phase_names[p], phase_sum[p] / n);
        strcat_s(phases, sizeof(phases), one);
    }
    if (g_Debug.phase_count) Debug_Log("profile", NULL, 0, "phases (ms avg): %s", phases);

    static ProfileRow rows[1024];
    int row_count = 0;
    for (DebugSite *site = g_Debug.sites; site && row_count < 1024; site = site->next) {
        double total = 0, smax = 0, hits = 0;
        for (int i = 0; i < n; i++) {
            long s = (last - i + Debug_ProfileFrames) % Debug_ProfileFrames;
            double ms = (double)site->cycles[s] / per_us / 1000.0;
            total += ms;
            if (ms > smax) smax = ms;
            hits += site->hits[s];
        }
        if (hits == 0) continue;
        ProfileRow r = { site, total / n, smax, hits / n, total * 1000.0 * per_us / hits };
        rows[row_count++] = r;
    }
    qsort(rows, (size_t)row_count, sizeof(ProfileRow), CompareRows);
    for (int i = 0; i < row_count; i++) {
        const char *base = strrchr(rows[i].site->file, '\\');
        base = base ? base + 1 : rows[i].site->file;
        Debug_Log("profile", NULL, 0, "%-28s %8.3f ms avg %8.3f max %8.1f hits %10.0f cy/hit  %s(%d)", rows[i].site->name,
                  rows[i].avg_ms, rows[i].max_ms, rows[i].hits, rows[i].cycles_per_hit, base, rows[i].site->line);
    }
}
