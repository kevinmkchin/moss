// term: the C side's base (term_core.h).
#include "term_core.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

// --- arena ---------------------------------------------------------------------------------------------------

enum { Arena_Reserve = 1 << 30, Arena_CommitStep = 1 << 16 };   // 1 GiB of address space, 64 KiB commits

void *Arena_Push(Arena *arena, u64 size, u64 align)
{
    if (!arena->base) {
        arena->base = (u8 *)VirtualAlloc(NULL, Arena_Reserve, MEM_RESERVE, PAGE_NOACCESS);
        arena->reserved = Arena_Reserve;
    }
    if (!align) align = 8;
    u64 at = (arena->used + align - 1) & ~(align - 1);
    u64 end = at + size;
    if (end > arena->reserved) { *(volatile int *)0 = 0; }   // past a gigabyte: a bug, stop here
    if (end > arena->committed) {
        u64 want = (end + Arena_CommitStep - 1) & ~(u64)(Arena_CommitStep - 1);
        VirtualAlloc(arena->base + arena->committed, want - arena->committed, MEM_COMMIT, PAGE_READWRITE);
        arena->committed = want;
    }
    arena->used = end;
    return arena->base + at;   // fresh commits are zero, and Arena_Clear zeroes what it hands back
}

void Arena_Clear(Arena *arena)
{
    if (arena->base) memset(arena->base, 0, (size_t)arena->used);
    arena->used = 0;
}

void Arena_Release(Arena *arena)
{
    if (arena->base) VirtualFree(arena->base, 0, MEM_RELEASE);
    Arena zero = {0};
    *arena = zero;
}

void *Array_Grow(Arena *arena, s64 *count_capacity, void *data, u64 item_size)
{
    s64 count = count_capacity[0], capacity = count_capacity[1];
    s64 grown = capacity ? capacity * 2 : 16;
    void *fresh = Arena_Push(arena, (u64)grown * item_size, 16);
    if (count) memcpy(fresh, data, (size_t)((u64)count * item_size));
    count_capacity[1] = grown;
    return fresh;
}

// --- strings -------------------------------------------------------------------------------------------------

String Str_FromCStr(const char *s)
{
    String r = { (u8 *)s, s ? (s64)strlen(s) : 0 };
    return r;
}

String Str_PushCopy(Arena *arena, String s)
{
    u8 *p = (u8 *)Arena_Push(arena, (u64)s.count + 1, 1);
    if (s.count) memcpy(p, s.data, (size_t)s.count);
    String r = { p, s.count };
    return r;
}

String Str_PushFormatV(Arena *arena, const char *fmt, va_list args)
{
    va_list again;
    va_copy(again, args);
    int n = vsnprintf(NULL, 0, fmt, args);
    if (n < 0) n = 0;
    u8 *p = (u8 *)Arena_Push(arena, (u64)n + 1, 1);
    vsnprintf((char *)p, (size_t)n + 1, fmt, again);
    va_end(again);
    String r = { p, n };
    return r;
}

String Str_PushFormat(Arena *arena, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    String r = Str_PushFormatV(arena, fmt, args);
    va_end(args);
    return r;
}

b32 Str_Equal(String a, String b)
{
    return a.count == b.count && (!a.count || memcmp(a.data, b.data, (size_t)a.count) == 0);
}

b32 Str_EqualNoCase(String a, String b)
{
    if (a.count != b.count) return 0;
    for (s64 i = 0; i < a.count; i++) {
        u8 x = a.data[i], y = b.data[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

void TB_Append(TextBuilder *tb, String s)
{
    if (tb->count + s.count > tb->capacity) {
        s64 grown = Max(tb->capacity * 2, tb->count + s.count + 64);
        u8 *fresh = (u8 *)Arena_Push(tb->arena, (u64)grown + 1, 1);
        if (tb->count) memcpy(fresh, tb->data, (size_t)tb->count);
        tb->data = fresh;
        tb->capacity = grown;
    }
    if (s.count) memcpy(tb->data + tb->count, s.data, (size_t)s.count);
    tb->count += s.count;
}

void TB_Format(TextBuilder *tb, const char *fmt, ...)
{
    char small[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(small, sizeof(small), fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n < (int)sizeof(small)) { String s = { (u8 *)small, n }; TB_Append(tb, s); return; }
    va_start(args, fmt);
    String s = Str_PushFormatV(tb->arena, fmt, args);
    va_end(args);
    TB_Append(tb, s);
}

String TB_String(TextBuilder *tb)
{
    String r = { tb->data, tb->count };
    return r;
}

// --- UTF-8 ---------------------------------------------------------------------------------------------------

u32 Utf8_Decode(String s, s64 *at)
{
    u8 c = s.data[*at];
    s32 n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!n || *at + n > s.count) { *at += 1; return 0xFFFD; }
    u32 cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (s32 k = 1; k < n; k++) {
        u8 b = s.data[*at + k];
        if ((b & 0xC0) != 0x80) { *at += 1; return 0xFFFD; }
        cp = (cp << 6) | (b & 0x3F);
    }
    *at += n;
    return cp;
}

s64 Utf8_Encode(u8 out[4], u32 cp)
{
    if (cp < 0x80) { out[0] = (u8)cp; return 1; }
    if (cp < 0x800) { out[0] = (u8)(0xC0 | (cp >> 6)); out[1] = (u8)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (u8)(0xE0 | (cp >> 12)); out[1] = (u8)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (u8)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (u8)(0xF0 | (cp >> 18)); out[1] = (u8)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (u8)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (u8)(0x80 | (cp & 0x3F));
    return 4;
}

WString Utf8ToUtf16(Arena *arena, String s)
{
    int n = s.count ? MultiByteToWideChar(CP_UTF8, 0, (const char *)s.data, (int)s.count, NULL, 0) : 0;
    u16 *p = (u16 *)Arena_Push(arena, (u64)(n + 1) * 2, 2);
    if (n) MultiByteToWideChar(CP_UTF8, 0, (const char *)s.data, (int)s.count, (wchar_t *)p, n);
    WString r = { p, n };
    return r;
}

String Utf16ToUtf8(Arena *arena, WString s)
{
    int n = s.count ? WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)s.data, (int)s.count, NULL, 0, NULL, NULL) : 0;
    u8 *p = (u8 *)Arena_Push(arena, (u64)n + 1, 1);
    if (n) WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)s.data, (int)s.count, (char *)p, n, NULL, NULL);
    String r = { p, n };
    return r;
}

// --- hash, time ----------------------------------------------------------------------------------------------

// 8 bytes at a time, each multiplied in and folded (a splitmix-style finish).
u64 Hash_Bytes(const void *data, u64 size, u64 seed)
{
    const u8 *p = (const u8 *)data;
    u64 h = seed ^ (size * 0x9E3779B97F4A7C15ull);
    while (size >= 8) {
        u64 v;
        memcpy(&v, p, 8);
        h = (h ^ (v * 0xBF58476D1CE4E5B9ull)) * 0x94D049BB133111EBull;
        h ^= h >> 31;
        p += 8;
        size -= 8;
    }
    u64 tail = 0;
    if (size) memcpy(&tail, p, (size_t)size);
    h = (h ^ (tail * 0xBF58476D1CE4E5B9ull) ^ size) * 0x94D049BB133111EBull;
    h ^= h >> 29;
    h *= 0xD6E8FEB86659FD93ull;
    h ^= h >> 32;
    return h;
}

u64 Time_NowMicros(void)
{
    static LARGE_INTEGER freq, start;
    LARGE_INTEGER now;
    if (!freq.QuadPart) { QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&start); }
    QueryPerformanceCounter(&now);
    return (u64)((now.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart);
}
