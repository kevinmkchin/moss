// term: run logs, asserts, the crash handler and the frame profiler. Used by every process (window,
// headless run, server) and from both C and C++.
//
// Logs: each run writes logs\YYYY-MM-DD_HHMMSS_<mode>_<pid>.log (build\logs next to a dev build's exe,
// else %LOCALAPPDATA%\moss\logs). Lines: "[frame] source: file(line): message". Log per event, never per
// frame or per item: hot paths use timed blocks.
//
// Crashes: a failed Assert/AssertAlways or a fatal exception writes the crash to the log (exception,
// symbolized stack), a minidump logs\<run>.dmp and copies of the exe and PDB in logs\<run>_symbols\.
//
// Profiling: BEGIN_TIMED_BLOCK(Name) / END_TIMED_BLOCK(Name) (C; END on every path out) or TIMED_BLOCK(Name)
// (C++, the rest of the scope). Each is summed into the current frame of a 240-frame ring; Debug_FrameEnd
// closes a frame; Debug_LogProfile writes "profile:" lines. With Superluminal's PerformanceAPI.dll present,
// each block is also a Superluminal event.
#ifndef TERM_DEBUG_H
#define TERM_DEBUG_H

#include <stdint.h>
#include <intrin.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { Debug_ProfileFrames = 240 };

typedef struct DebugSite {
    const char *name;
    const char *file;
    int line;
    volatile long registered;
    struct DebugSite *next;
    volatile int64_t cycles[Debug_ProfileFrames];
    volatile long hits[Debug_ProfileFrames];
} DebugSite;

// mode: "window", "headless", "server", "replay". echo_stdout: also write each line to stdout.
void Debug_Init(const char *mode, int echo_stdout);
void Debug_SetFrame(uint32_t frame);   // the [frame] of later lines (the loop's frame or tick)
void Debug_Log(const char *source, const char *file, int line, const char *fmt, ...);
void Debug_LogHresult(const char *source, const char *file, int line, const char *what, long hr);
const char *Debug_LogPath(void);
void Debug_AssertFailed(const char *expr, const char *file, int line);
void Debug_Crash(void);   // the script's "crash": an AssertAlways that fails

// Profiler.
void Debug_RecordBlock(DebugSite *site, uint64_t cycles);
// Closes the frame: its work time (from the start of its work to its end, waits excluded) and phase
// times, in microseconds (phase names are set once with Debug_SetPhaseNames, at most 8).
void Debug_SetPhaseNames(const char *const *names, int count);
void Debug_FrameEnd(uint64_t work_us, const uint64_t *phase_us);
// "profile:" lines over the last `frames` closed frames (at most Debug_ProfileFrames).
void Debug_LogProfile(int frames, const char *why);
// Frames whose work took longer than this log a one-frame profile (up to 5 in a row, then once a second).
void Debug_SetSlowFrameUs(uint64_t us);
// Work time of the last `frames` closed frames, in ms: average and worst.
void Debug_RecentWork(int frames, float *avg_ms, float *max_ms);

// Superluminal (PerformanceAPI.dll, loaded at init when it's installed; never required).
void Debug_PerfBegin(const char *name);
void Debug_PerfEnd(void);
void Debug_NameThread(const char *name);
extern int g_DebugPerfApi;

#ifdef __cplusplus
}
#endif

#define Logf(source, ...) Debug_Log((source), __FILE__, __LINE__, __VA_ARGS__)
#define LogHr(source, what, hr) Debug_LogHresult((source), __FILE__, __LINE__, (what), (long)(hr))

#define AssertAlways(expr) do { if (!(expr)) { Debug_AssertFailed(#expr, __FILE__, __LINE__); *(volatile int *)0 = 0; } } while (0)
#if TERM_INTERNAL
#define Assert(expr) AssertAlways(expr)
#else
#define Assert(expr) ((void)0)
#endif

#define BEGIN_TIMED_BLOCK(Name) \
    static DebugSite DebugSite_##Name = { #Name, __FILE__, __LINE__ }; \
    if (g_DebugPerfApi) Debug_PerfBegin(#Name); \
    uint64_t DebugStart_##Name = __rdtsc()
#define END_TIMED_BLOCK(Name) \
    do { Debug_RecordBlock(&DebugSite_##Name, __rdtsc() - DebugStart_##Name); if (g_DebugPerfApi) Debug_PerfEnd(); } while (0)

#ifdef __cplusplus
struct debug_timed_block
{
    DebugSite *Site;
    uint64_t Start;
    debug_timed_block(DebugSite *S) : Site(S), Start(__rdtsc()) { if (g_DebugPerfApi) Debug_PerfBegin(S->name); }
    ~debug_timed_block() { Debug_RecordBlock(Site, __rdtsc() - Start); if (g_DebugPerfApi) Debug_PerfEnd(); }
};
#define TIMED_BLOCK(Name) \
    static DebugSite DebugSite_##Name = { #Name, __FILE__, __LINE__ }; \
    debug_timed_block DebugBlock_##Name(&DebugSite_##Name)
#endif

#endif
