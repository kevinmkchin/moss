// term: folder listings, watches and folder sizes (term_files.h). Three threads:
//   - the lister lists folders in request order;
//   - the watcher waits on ReadDirectoryChangesW per watched folder: a change at its top level means "list it
//     again"; in a folder whose sizes are measured it watches the whole tree, and a change deeper down only marks
//     the subfolder it's in as changed;
//   - the sizer measures subfolders, one at a time, in Windows' background mode (low CPU and I/O priority): every
//     result is cached, a changed subfolder is measured again once it's been quiet a while, never more than once
//     per 30 s, and a measure stops as soon as its folder isn't shown. Local SSDs only, never a drive's root.
#include "term_files.h"
#include <winioctl.h>

typedef struct FilesQueued { FileRequest r; struct FilesQueued *next; } FilesQueued;
typedef struct FilesDone { FileListing l; Arena arena; struct FilesDone *next; } FilesDone;
typedef struct FilesChanged { String path; struct FilesChanged *next; } FilesChanged;
typedef struct FilesSized { FolderSize s; struct FilesSized *next; } FilesSized;

// A measured subfolder, by the hash of its path (lowercase).
typedef struct SizeRecord {
    u64 key;
    u64 bytes, files, folders;
    u64 measured_ms;          // 0: never
    u64 changed_ms;           // something under it changed then (0: nothing since it was measured)
} SizeRecord;
enum { Size_Records = 1 << 15, Files_MaxWatches = 48 };

// Online-only placeholders (OneDrive and other cloud files): opening or listing their contents downloads them.
enum { Attr_OnlineOnly = FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS };

typedef struct WatchWish {
    String path;              // heap copy, as the app gave it
    b32 sizes;
    b32 fresh;                // new: the sizer sends every subfolder's size once
    u64 all_changed_ms;       // the watcher lost track (its buffer overflowed): every subfolder counts as changed
    u64 since_ms;             // shown (watched) since then
} WatchWish;

static struct {
    CRITICAL_SECTION lock;
    HANDLE work, watch_wake, size_wake;   // events: a request waits; the watched set changed; the sizer has work
    HANDLE threads[3];
    volatile LONG stop;
    HWND wake_window;
    u32 wake_message;
    FilesQueued *queue, *queue_tail;
    FilesDone *done, *done_tail;
    FilesChanged *changed;
    FilesSized *sized;
    WatchWish watch[Files_MaxWatches];    // what the app wants watched
    s32 watch_count;
    SizeRecord *records;                  // Size_Records, open addressing (under lock)
} g_Files;

static void FilesWake(void)
{
    if (g_Files.wake_window) PostMessageW(g_Files.wake_window, g_Files.wake_message, 0, 0);
}

static String HeapCopy(String s)
{
    String r = { (u8 *)malloc((size_t)s.count + 1), s.count };
    memcpy(r.data, s.data, (size_t)s.count);
    r.data[s.count] = 0;
    return r;
}

static u64 NowMs(void) { return GetTickCount64(); }

// The key of folder\name (case folded, the folder's trailing separator dropped).
static u64 SizeKey(String folder, String name)
{
    while (folder.count && folder.data[folder.count - 1] == '\\') folder.count--;
    u64 h = 0x9E3779B97F4A7C15ull;
    for (s32 part = 0; part < 3; part++) {
        String s = part == 0 ? folder : part == 1 ? S("\\") : name;
        for (s64 i = 0; i < s.count; i++) {
            u8 c = s.data[i];
            h = (h ^ (u8)(c >= 'A' && c <= 'Z' ? c + 32 : c)) * 0x100000001b3ull;
        }
    }
    return h | 1;
}

// The record of key (made when missing). Under lock.
static SizeRecord *SizeRecordFor(u64 key)
{
    u32 at = (u32)(key % Size_Records);
    for (u32 probe = 0; probe < 64; probe++) {
        SizeRecord *r = &g_Files.records[(at + probe) % Size_Records];
        if (r->key == key) return r;
        if (!r->key) { r->key = key; return r; }
    }
    // a crowded neighbourhood: the oldest of it goes
    SizeRecord *oldest = &g_Files.records[at];
    for (u32 probe = 1; probe < 64; probe++) {
        SizeRecord *r = &g_Files.records[(at + probe) % Size_Records];
        if (r->measured_ms < oldest->measured_ms) oldest = r;
    }
    SizeRecord zero = {0};
    *oldest = zero;
    oldest->key = key;
    return oldest;
}

// --- the lister ----------------------------------------------------------------------------------------------

static void ListDrives(FilesDone *d)
{
    WCHAR drives[512] = {0};
    GetLogicalDriveStringsW(511, drives);
    s64 n = 0;
    for (WCHAR *at = drives; *at; at += wcslen(at) + 1) n++;
    d->l.entries = Arena_PushArray(&d->arena, FileEntry, n + 1);
    for (WCHAR *at = drives; *at; at += wcslen(at) + 1) {
        FileEntry *e = &d->l.entries[d->l.count++];
        e->name = Utf16ToUtf8(&d->arena, (WString){ (u16 *)at, (s64)wcslen(at) });
        e->flags = File_Dir | File_Drive;
        UINT type = GetDriveTypeW(at);
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
            // network and optical drives can take seconds to answer: only local ones are asked
            WCHAR label[MAX_PATH] = {0};
            if (GetVolumeInformationW(at, label, MAX_PATH, NULL, NULL, NULL, NULL, 0)) e->label = Utf16ToUtf8(&d->arena, (WString){ (u16 *)label, (s64)wcslen(label) });
            ULARGE_INTEGER free_bytes = {0}, total = {0};
            if (GetDiskFreeSpaceExW(at, &free_bytes, &total, NULL)) { e->size = total.QuadPart; e->free = free_bytes.QuadPart; }
        }
    }
}

static void ListFolder(FilesDone *d)
{
    String pattern = Str_PushFormat(&d->arena, "%.*s%s*", STR_ARG(d->l.path), d->l.path.count && d->l.path.data[d->l.path.count - 1] == '\\' ? "" : "\\");
    WString wp = Utf8ToUtf16(&d->arena, pattern);
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileExW((const WCHAR *)wp.data, FindExInfoBasic, &f, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) { d->l.error = (s32)GetLastError(); return; }
    Array(FileEntry) list = {0};
    do {
        if (f.cFileName[0] == '.' && (!f.cFileName[1] || (f.cFileName[1] == '.' && !f.cFileName[2]))) continue;
        FileEntry *e = Array_Push(&d->arena, list);
        e->name = Utf16ToUtf8(&d->arena, (WString){ (u16 *)f.cFileName, (s64)wcslen(f.cFileName) });
        e->size = ((u64)f.nFileSizeHigh << 32) | f.nFileSizeLow;
        e->modified = ((u64)f.ftLastWriteTime.dwHighDateTime << 32) | f.ftLastWriteTime.dwLowDateTime;
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) e->flags |= File_Dir;
        if ((f.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) || f.cFileName[0] == '.') e->flags |= File_Hidden;   // dotfiles too, as on Unix
        // links: symlinks and junctions only (cloud files are reparse points too, with their own tags)
        if ((f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && (f.dwReserved0 == IO_REPARSE_TAG_SYMLINK || f.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)) e->flags |= File_Link;
        if (f.dwFileAttributes & Attr_OnlineOnly) e->flags |= File_Cloud;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    d->l.entries = list.data;
    d->l.count = list.count;
}

static DWORD WINAPI FilesListThread(void *param)
{
    (void)param;
    while (!g_Files.stop) {
        WaitForSingleObject(g_Files.work, INFINITE);
        for (;;) {
            EnterCriticalSection(&g_Files.lock);
            FilesQueued *q = g_Files.queue;
            if (q) { g_Files.queue = q->next; if (!g_Files.queue) g_Files.queue_tail = NULL; }
            LeaveCriticalSection(&g_Files.lock);
            if (!q || g_Files.stop) break;
            FilesDone *d = (FilesDone *)calloc(1, sizeof(FilesDone));
            d->l.request = q->r.request;
            d->l.path = Str_PushCopy(&d->arena, q->r.path);
            if (q->r.path.count) ListFolder(d); else ListDrives(d);
            free(q->r.path.data);
            free(q);
            EnterCriticalSection(&g_Files.lock);
            if (g_Files.done_tail) g_Files.done_tail->next = d; else g_Files.done = d;
            g_Files.done_tail = d;
            LeaveCriticalSection(&g_Files.lock);
            FilesWake();
        }
    }
    return 0;
}

static b32 SizableFolder(String folder);

// --- the watcher -----------------------------------------------------------------------------------------------

typedef struct WatchSlot {
    String path;              // heap
    b32 tree;                 // the whole tree (sizes), else the folder's own entries
    HANDLE dir;
    OVERLAPPED ov;
    u8 *buf;                  // 64 KiB of FILE_NOTIFY_INFORMATION
} WatchSlot;

static b32 WatchStart(WatchSlot *s)
{
    DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                   FILE_NOTIFY_CHANGE_ATTRIBUTES;
    ResetEvent(s->ov.hEvent);
    return ReadDirectoryChangesW(s->dir, s->buf, 1 << 16, s->tree, filter, NULL, &s->ov, NULL) != 0;
}

static void WatchClose(WatchSlot *s)
{
    CancelIoEx(s->dir, &s->ov);
    DWORD ignored = 0;
    GetOverlappedResult(s->dir, &s->ov, &ignored, TRUE);
    CloseHandle(s->dir);
    CloseHandle(s->ov.hEvent);
    free(s->buf);
    free(s->path.data);
}

// What a batch of changes in slot s means: the folder's own entries changed (list it again), or something under
// one of its subfolders did (that subfolder's size is out of date).
static void WatchApply(WatchSlot *s, DWORD bytes)
{
    b32 listing = bytes == 0;   // 0: the buffer overflowed, everything counts as changed
    u64 now = NowMs();
    Arena scratch = {0};
    EnterCriticalSection(&g_Files.lock);
    if (!bytes) {
        for (s32 i = 0; i < g_Files.watch_count; i++) if (Str_EqualNoCase(g_Files.watch[i].path, s->path)) g_Files.watch[i].all_changed_ms = now;
    }
    for (DWORD at = 0; bytes;) {
        FILE_NOTIFY_INFORMATION *n = (FILE_NOTIFY_INFORMATION *)(s->buf + at);
        s64 len = (s64)(n->FileNameLength / 2), cut = 0;
        while (cut < len && n->FileName[cut] != L'\\') cut++;
        if (cut == len) {
            listing = 1;
        } else {
            String child = Utf16ToUtf8(&scratch, (WString){ (u16 *)n->FileName, cut });
            SizeRecord *r = SizeRecordFor(SizeKey(s->path, child));
            r->changed_ms = now;
        }
        if (!n->NextEntryOffset) break;
        at += n->NextEntryOffset;
    }
    if (listing) {
        FilesChanged *c = (FilesChanged *)calloc(1, sizeof(FilesChanged));
        c->path = HeapCopy(s->path);
        c->next = g_Files.changed;
        g_Files.changed = c;
    }
    LeaveCriticalSection(&g_Files.lock);
    Arena_Release(&scratch);
    if (listing) FilesWake();
    if (s->tree) SetEvent(g_Files.size_wake);
}

static DWORD WINAPI FilesWatchThread(void *param)
{
    (void)param;
    WatchSlot slots[MAXIMUM_WAIT_OBJECTS - 1];
    s32 count = 0;
    while (!g_Files.stop) {
        HANDLE handles[MAXIMUM_WAIT_OBJECTS];
        handles[0] = g_Files.watch_wake;
        for (s32 i = 0; i < count; i++) handles[1 + i] = slots[i].ov.hEvent;
        DWORD r = WaitForMultipleObjects((DWORD)count + 1, handles, FALSE, INFINITE);
        if (g_Files.stop) break;
        if (r == WAIT_OBJECT_0) {
            // --- the set changed: drop what's no longer wanted (or wanted another way), add what's new ---
            EnterCriticalSection(&g_Files.lock);
            WatchWish wanted[Files_MaxWatches];
            s32 wanted_count = g_Files.watch_count;
            for (s32 k = 0; k < wanted_count; k++) { wanted[k] = g_Files.watch[k]; wanted[k].path = HeapCopy(g_Files.watch[k].path); }
            for (s32 k = 0; k < wanted_count; k++) wanted[k].sizes = wanted[k].sizes && SizableFolder(wanted[k].path);   // the whole tree only where sizes are measured
            LeaveCriticalSection(&g_Files.lock);
            for (s32 i = 0; i < count;) {
                b32 keep = 0;
                for (s32 k = 0; k < wanted_count && !keep; k++) keep = Str_EqualNoCase(wanted[k].path, slots[i].path) && !wanted[k].sizes == !slots[i].tree;
                if (keep) { i++; continue; }
                WatchClose(&slots[i]);
                slots[i] = slots[--count];
            }
            for (s32 k = 0; k < wanted_count && count < (s32)ArrayCount(slots); k++) {
                b32 have = 0;
                for (s32 i = 0; i < count && !have; i++) have = Str_EqualNoCase(wanted[k].path, slots[i].path) && !wanted[k].sizes == !slots[i].tree;
                if (have) continue;
                Arena scratch = {0};
                WString w = Utf8ToUtf16(&scratch, wanted[k].path);
                HANDLE dir = CreateFileW((const WCHAR *)w.data, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
                Arena_Release(&scratch);
                if (dir == INVALID_HANDLE_VALUE) continue;
                WatchSlot *s = &slots[count];
                memset(s, 0, sizeof(*s));
                s->path = HeapCopy(wanted[k].path);
                s->tree = wanted[k].sizes;
                s->dir = dir;
                s->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
                s->buf = (u8 *)malloc(1 << 16);
                if (WatchStart(s)) count++;
                else WatchClose(s);
            }
            for (s32 k = 0; k < wanted_count; k++) free(wanted[k].path.data);
            continue;
        }
        s32 i = (s32)(r - WAIT_OBJECT_0) - 1;
        if (i < 0 || i >= count) { Sleep(50); continue; }
        DWORD bytes = 0;
        if (!GetOverlappedResult(slots[i].dir, &slots[i].ov, &bytes, FALSE)) {
            // the folder went away (deleted, a drive pulled): its watch goes too
            WatchClose(&slots[i]);
            slots[i] = slots[--count];
            continue;
        }
        WatchApply(&slots[i], bytes);
        if (!WatchStart(&slots[i])) { WatchClose(&slots[i]); slots[i] = slots[--count]; continue; }
        Sleep(30);   // a burst of changes (a build, an unzip) wakes the app a few times, not thousands
    }
    for (s32 i = 0; i < count; i++) WatchClose(&slots[i]);
    return 0;
}

// --- the sizer -------------------------------------------------------------------------------------------------

// FILE_DIRECTORY_INFORMATION, as NtQueryDirectoryFile fills it (class 1).
typedef struct DirInfo {
    ULONG NextEntryOffset, FileIndex;
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize;
    ULONG FileAttributes, FileNameLength;
    WCHAR FileName[1];
} DirInfo;
typedef struct IoStatus { union { LONG status; void *pointer; }; ULONG_PTR information; } IoStatus;
typedef LONG (NTAPI *NtQueryDirectoryFileFn)(HANDLE, HANDLE, void *, void *, IoStatus *, void *, ULONG, int, BOOLEAN, void *, BOOLEAN);

// A folder still shown with its sizes wanted (a measure of it stops when it isn't).
static b32 SizesWanted(String folder)
{
    b32 wanted = 0;
    EnterCriticalSection(&g_Files.lock);
    for (s32 i = 0; i < g_Files.watch_count && !wanted; i++) wanted = g_Files.watch[i].sizes && Str_EqualNoCase(g_Files.watch[i].path, folder);
    LeaveCriticalSection(&g_Files.lock);
    return wanted;
}

// Sizes are measured in folders on a local SSD (no seek penalty: what File Pilot measures by default too), below
// the drive's root (whose folders are the whole disk). Each drive asked once.
static b32 SizableFolder(String folder)
{
    static u8 known[26];   // 0 not asked, 1 yes, 2 no
    if (folder.count < 4 || folder.data[1] != ':' || folder.data[2] != '\\') return 0;
    u8 letter = (u8)toupper(folder.data[0]);
    if (letter < 'A' || letter > 'Z') return 0;
    u8 *k = &known[letter - 'A'];
    if (!*k) {
        WCHAR root[4] = { (WCHAR)letter, L':', L'\\', 0 }, device[8] = { L'\\', L'\\', L'.', L'\\', (WCHAR)letter, L':', 0 };
        b32 ssd = 0;
        if (GetDriveTypeW(root) == DRIVE_FIXED) {
            HANDLE h = CreateFileW(device, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                STORAGE_PROPERTY_QUERY q = { StorageDeviceSeekPenaltyProperty, PropertyStandardQuery, {0} };
                DEVICE_SEEK_PENALTY_DESCRIPTOR d = {0};
                DWORD got = 0;
                ssd = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &d, sizeof(d), &got, NULL) && !d.IncursSeekPenalty;
                CloseHandle(h);
            }
        }
        *k = ssd ? 1 : 2;
        Logf("files", "drive %c: %s", letter, ssd ? "a local SSD: folder sizes on" : "not a local SSD: no folder sizes");
    }
    return *k == 1;
}

// Everything under `path`: bytes, files and folders (links counted, never followed). NtQueryDirectoryFile hands
// back a directory's entries 64 KiB at a time, far fewer calls than FindNextFile. 0 when it stopped because
// `watched` isn't shown any more.
static b32 MeasureFolder(NtQueryDirectoryFileFn query, String path, String watched, u64 out[3], u64 *entries)
{
    Arena a = {0};
    Array(WString) stack = {0};
    *Array_Push(&a, stack) = Utf8ToUtf16(&a, path);
    u8 *buf = (u8 *)Arena_Push(&a, 1 << 16, 8);
    b32 done = 1;
    for (s64 visited = 0; stack.count; visited++) {
        if (visited % 64 == 63 && !SizesWanted(watched)) { done = 0; break; }
        WString dir = stack.data[--stack.count];
        HANDLE h = CreateFileW((const WCHAR *)dir.data, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        for (BOOLEAN restart = TRUE;; restart = FALSE) {
            IoStatus io = {0};
            if (query(h, NULL, NULL, NULL, &io, buf, 1 << 16, 1, FALSE, NULL, restart) < 0) break;   // the end, or an error
            for (DirInfo *e = (DirInfo *)buf;; e = (DirInfo *)((u8 *)e + e->NextEntryOffset)) {
                s64 len = (s64)(e->FileNameLength / 2);
                b32 dots = (len == 1 && e->FileName[0] == L'.') || (len == 2 && e->FileName[0] == L'.' && e->FileName[1] == L'.');
                if (!dots) {
                    (*entries)++;
                    if (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        out[2]++;
                        if (!(e->FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | Attr_OnlineOnly))) {   // links and cloud placeholders: counted, never entered
                            WString sub = { Arena_PushArray(&a, u16, dir.count + len + 2), dir.count + 1 + len };
                            memcpy(sub.data, dir.data, (size_t)dir.count * 2);
                            sub.data[dir.count] = L'\\';
                            memcpy(sub.data + dir.count + 1, e->FileName, (size_t)len * 2);
                            *Array_Push(&a, stack) = sub;
                        }
                    } else {
                        out[1]++;
                        out[0] += (u64)e->EndOfFile.QuadPart;
                    }
                }
                if (!e->NextEntryOffset) break;
            }
        }
        CloseHandle(h);
    }
    Arena_Release(&a);
    return done;
}

static void SizePost(String folder, String name, const SizeRecord *r)
{
    FilesSized *s = (FilesSized *)calloc(1, sizeof(FilesSized));
    s->s.folder = HeapCopy(folder);
    s->s.name = HeapCopy(name);
    s->s.bytes = r->bytes;
    s->s.files = r->files;
    s->s.folders = r->folders;
    EnterCriticalSection(&g_Files.lock);
    s->next = g_Files.sized;
    g_Files.sized = s;
    LeaveCriticalSection(&g_Files.lock);
    FilesWake();
}

static DWORD WINAPI FilesSizeThread(void *param)
{
    (void)param;
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   // low CPU and I/O priority: never in the way
    NtQueryDirectoryFileFn query = (NtQueryDirectoryFileFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryDirectoryFile");
    DWORD wait = INFINITE;
    while (!g_Files.stop && query) {
        WaitForSingleObject(g_Files.size_wake, wait);
        if (g_Files.stop) break;
        wait = INFINITE;
        // --- what's wanted now ---
        WatchWish wishes[Files_MaxWatches];
        s32 wish_count = 0;
        EnterCriticalSection(&g_Files.lock);
        for (s32 i = 0; i < g_Files.watch_count; i++) {
            if (!g_Files.watch[i].sizes) continue;
            wishes[wish_count] = g_Files.watch[i];
            wishes[wish_count++].path = HeapCopy(g_Files.watch[i].path);
            g_Files.watch[i].fresh = 0;
        }
        LeaveCriticalSection(&g_Files.lock);
        for (s32 w = 0; w < wish_count && !g_Files.stop; w++) {
            WatchWish *wish = &wishes[w];
            if (!SizableFolder(wish->path)) continue;
            // --- its subfolders (not links) ---
            Arena names = {0};
            Array(String) subs = {0};
            WString pattern = Utf8ToUtf16(&names, Str_PushFormat(&names, "%.*s\\*", STR_ARG(wish->path)));
            WIN32_FIND_DATAW fd;
            HANDLE fh = FindFirstFileExW((const WCHAR *)pattern.data, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    b32 dots = fd.cFileName[0] == '.' && (!fd.cFileName[1] || (fd.cFileName[1] == '.' && !fd.cFileName[2]));
                    if (dots || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | Attr_OnlineOnly))) continue;
                    *Array_Push(&names, subs) = Utf16ToUtf8(&names, (WString){ (u16 *)fd.cFileName, (s64)wcslen(fd.cFileName) });
                } while (FindNextFileW(fh, &fd));
                FindClose(fh);
            }
            // --- each one: sent from the cache when it's good, measured when it's missing, old, or changed and quiet
            //     since (never twice in 30 s) ---
            u64 started = NowMs(), entries = 0, cpu0 = 0, cpu1 = 0, dummy = 0;
            s32 measured = 0;
            u64 kernel0 = 0, kernel1 = 0;
            GetThreadTimes(GetCurrentThread(), (FILETIME *)&dummy, (FILETIME *)&dummy, (FILETIME *)&kernel0, (FILETIME *)&cpu0);
            b32 stopped = 0;
            for (s64 i = 0; i < subs.count && !stopped && !g_Files.stop; i++) {
                u64 now = NowMs(), key = SizeKey(wish->path, subs.data[i]);
                EnterCriticalSection(&g_Files.lock);
                SizeRecord r = *SizeRecordFor(key);
                LeaveCriticalSection(&g_Files.lock);
                b32 changed = r.changed_ms > r.measured_ms || wish->all_changed_ms > r.measured_ms;
                // measured while this folder was shown: its watch saw every change since; before: good for 5 minutes
                b32 good = r.measured_ms && !changed && (r.measured_ms >= wish->since_ms || now - r.measured_ms < 5 * 60 * 1000);
                if (r.measured_ms && wish->fresh) SizePost(wish->path, subs.data[i], &r);   // what we know, meanwhile
                if (good) continue;
                if (r.measured_ms && changed) {
                    u64 quiet = Max(r.changed_ms, wish->all_changed_ms) + 2000;
                    u64 due = Max(r.measured_ms + 30000, Min(quiet, r.measured_ms + 60000));
                    if (now < due) {
                        DWORD in = (DWORD)(due - now);
                        if (wait == INFINITE || in < wait) wait = in;
                        continue;
                    }
                }
                u64 totals[3] = {0};
                String full = Str_PushFormat(&names, "%.*s\\%.*s", STR_ARG(wish->path), STR_ARG(subs.data[i]));
                if (!MeasureFolder(query, full, wish->path, totals, &entries)) { stopped = 1; break; }
                measured++;
                EnterCriticalSection(&g_Files.lock);
                SizeRecord *rec = SizeRecordFor(key);
                rec->bytes = totals[0];
                rec->files = totals[1];
                rec->folders = totals[2];
                rec->measured_ms = NowMs();
                r = *rec;
                LeaveCriticalSection(&g_Files.lock);
                SizePost(wish->path, subs.data[i], &r);
            }
            GetThreadTimes(GetCurrentThread(), (FILETIME *)&dummy, (FILETIME *)&dummy, (FILETIME *)&kernel1, (FILETIME *)&cpu1);
            cpu1 += kernel1 - kernel0;   // the system calls are most of it
            if (measured || stopped) {
                Logf("files", "sizes: %.*s: %d of %lld folders measured, %llu entries, %llu ms, cpu %llu ms%s", STR_ARG(wish->path), measured, (long long)subs.count,
                     (unsigned long long)entries, (unsigned long long)(NowMs() - started), (unsigned long long)((cpu1 - cpu0) / 10000), stopped ? " (stopped: not shown)" : "");
            }
            Arena_Release(&names);
        }
        for (s32 w = 0; w < wish_count; w++) free(wishes[w].path.data);
    }
    return 0;
}

// --- the app's side -----------------------------------------------------------------------------------------------

void Files_Start(void *wake_window, u32 wake_message)
{
    InitializeCriticalSection(&g_Files.lock);
    g_Files.wake_window = (HWND)wake_window;
    g_Files.wake_message = wake_message;
    g_Files.records = (SizeRecord *)calloc(Size_Records, sizeof(SizeRecord));
    g_Files.work = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_Files.watch_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_Files.size_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_Files.threads[0] = CreateThread(NULL, 0, FilesListThread, NULL, 0, NULL);
    g_Files.threads[1] = CreateThread(NULL, 0, FilesWatchThread, NULL, 0, NULL);
    g_Files.threads[2] = CreateThread(NULL, 0, FilesSizeThread, NULL, 0, NULL);
    if (g_Files.threads[0]) SetThreadPriority(g_Files.threads[0], THREAD_PRIORITY_BELOW_NORMAL);
}

void Files_Request(const FileRequest *r)
{
    FilesQueued *q = (FilesQueued *)calloc(1, sizeof(FilesQueued));
    q->r.request = r->request;
    q->r.path = HeapCopy(r->path);
    EnterCriticalSection(&g_Files.lock);
    if (g_Files.queue_tail) g_Files.queue_tail->next = q; else g_Files.queue = q;
    g_Files.queue_tail = q;
    LeaveCriticalSection(&g_Files.lock);
    SetEvent(g_Files.work);
}

void Files_Watch(const FileWatch *watches, s64 count)
{
    EnterCriticalSection(&g_Files.lock);
    b32 same = count == g_Files.watch_count;
    for (s64 i = 0; i < count && same; i++) same = Str_EqualNoCase(watches[i].path, g_Files.watch[i].path) && !watches[i].sizes == !g_Files.watch[i].sizes;
    if (!same) {
        // the new set; a wish that was already there keeps its state
        WatchWish next[Files_MaxWatches];
        s32 next_count = 0;
        for (s64 i = 0; i < count && next_count < Files_MaxWatches; i++) {
            if (!watches[i].path.count) continue;
            WatchWish *w = &next[next_count++];
            memset(w, 0, sizeof(*w));
            w->sizes = watches[i].sizes;
            w->fresh = 1;
            w->since_ms = NowMs();
            for (s32 k = 0; k < g_Files.watch_count; k++) {
                if (!Str_EqualNoCase(g_Files.watch[k].path, watches[i].path) || !g_Files.watch[k].sizes != !watches[i].sizes) continue;
                *w = g_Files.watch[k];
                w->path.data = NULL;
            }
            w->path = HeapCopy(watches[i].path);
        }
        for (s32 i = 0; i < g_Files.watch_count; i++) free(g_Files.watch[i].path.data);
        memcpy(g_Files.watch, next, sizeof(WatchWish) * (size_t)next_count);
        g_Files.watch_count = next_count;
    }
    LeaveCriticalSection(&g_Files.lock);
    if (!same) { SetEvent(g_Files.watch_wake); SetEvent(g_Files.size_wake); }
}

void Files_Drain(FilesIn *in, Arena *a)
{
    FilesIn zero = {0};
    *in = zero;
    EnterCriticalSection(&g_Files.lock);
    FilesDone *done = g_Files.done;
    FilesChanged *changed = g_Files.changed;
    FilesSized *sized = g_Files.sized;
    g_Files.done = g_Files.done_tail = NULL;
    g_Files.changed = NULL;
    g_Files.sized = NULL;
    LeaveCriticalSection(&g_Files.lock);
    for (FilesDone *d = done; d; d = d->next) in->listing_count++;
    for (FilesChanged *c = changed; c; c = c->next) in->changed_count++;
    for (FilesSized *s = sized; s; s = s->next) in->size_count++;
    in->listings = Arena_PushArray(a, FileListing, in->listing_count + 1);
    in->changed = Arena_PushArray(a, String, in->changed_count + 1);
    in->sizes = Arena_PushArray(a, FolderSize, in->size_count + 1);
    s64 n = 0;
    while (done) {
        FilesDone *d = done;
        done = d->next;
        FileListing *l = &in->listings[n++];
        *l = d->l;
        l->path = Str_PushCopy(a, d->l.path);
        l->entries = Arena_PushArray(a, FileEntry, d->l.count + 1);
        for (s64 i = 0; i < d->l.count; i++) {
            l->entries[i] = d->l.entries[i];
            l->entries[i].name = Str_PushCopy(a, d->l.entries[i].name);
            l->entries[i].label = Str_PushCopy(a, d->l.entries[i].label);
        }
        Arena_Release(&d->arena);
        free(d);
    }
    n = 0;
    while (changed) {
        FilesChanged *c = changed;
        changed = c->next;
        b32 dup = 0;
        for (s64 k = 0; k < n && !dup; k++) dup = Str_EqualNoCase(in->changed[k], c->path);
        if (!dup) in->changed[n++] = Str_PushCopy(a, c->path);
        free(c->path.data);
        free(c);
    }
    in->changed_count = n;
    n = in->size_count;   // newest first in the list: filled from the back, so the newest of a folder lands last
    while (sized) {
        FilesSized *s = sized;
        sized = s->next;
        FolderSize *o = &in->sizes[--n];
        *o = s->s;
        o->folder = Str_PushCopy(a, s->s.folder);
        o->name = Str_PushCopy(a, s->s.name);
        free(s->s.folder.data);
        free(s->s.name.data);
        free(s);
    }
}

void Files_Stop(void)
{
    if (!g_Files.work) return;
    g_Files.stop = 1;
    SetEvent(g_Files.work);
    SetEvent(g_Files.watch_wake);
    SetEvent(g_Files.size_wake);
    WaitForMultipleObjects(3, g_Files.threads, TRUE, 1000);
}
