// term: the window side's base: types, arenas, strings, UTF-8, hashing, time. C and C++ (the C++ side has its own
// Min / Max / Clamp in term_base.h).
#ifndef TERM_CORE_H
#define TERM_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef float    f32;
typedef double   f64;
typedef s32      b32;

#ifndef __cplusplus
#define ArrayCount(a) ((s64)(sizeof(a) / sizeof((a)[0])))
#define Min(a, b) ((a) < (b) ? (a) : (b))
#define Max(a, b) ((a) > (b) ? (a) : (b))
#define Clamp(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#endif
#define Kilobytes(n) ((u64)(n) << 10)
#define Megabytes(n) ((u64)(n) << 20)

#ifdef __cplusplus
#define TERM_ALIGNOF(T) alignof(T)
extern "C" {
#else
#define TERM_ALIGNOF(T) _Alignof(T)
#endif

// --- arena: one reserved address range, committed as it fills. A zeroed Arena is ready to use; pushes come back
//     zeroed (Arena_Clear zeroes what was used) ---
typedef struct Arena {
    u8 *base;
    u64 used, committed, reserved;
} Arena;

void *Arena_Push(Arena *arena, u64 size, u64 align);
void  Arena_Clear(Arena *arena);
void  Arena_Release(Arena *arena);
#define Arena_PushStruct(arena, T) ((T *)Arena_Push((arena), sizeof(T), TERM_ALIGNOF(T)))
#define Arena_PushArray(arena, T, n) ((T *)Arena_Push((arena), sizeof(T) * (u64)(n), TERM_ALIGNOF(T)))

// A growable array in an arena: {count, capacity, data}. A full one moves to a block twice the size (the old one
// stays behind in the arena).
#define Array(T) struct { s64 count; s64 capacity; T *data; }
void *Array_Grow(Arena *arena, s64 *count_capacity, void *data, u64 item_size);
#define Array_Push(arena, arr) \
    ((arr).count == (arr).capacity ? (void)((arr).data = Array_Grow((arena), &(arr).count, (arr).data, sizeof(*(arr).data))) : (void)0, \
     &(arr).data[(arr).count++])

// --- strings: UTF-8 slices, not necessarily NUL-terminated ---
typedef struct String  { u8  *data; s64 count; } String;
typedef struct WString { u16 *data; s64 count; } WString;   // UTF-16, for Win32

#ifdef __cplusplus
#define S(lit) (String{ (u8 *)(lit), (s64)(sizeof(lit) - 1) })
#else
#define S(lit) ((String){ (u8 *)(lit), (s64)(sizeof(lit) - 1) })
#endif
#define STR_ARG(s) (int)(s).count, (const char *)(s).data

String Str_FromCStr(const char *s);
String Str_PushCopy(Arena *arena, String s);                 // NUL-terminated
String Str_PushFormat(Arena *arena, const char *fmt, ...);   // printf; "%.*s" with STR_ARG for slices
String Str_PushFormatV(Arena *arena, const char *fmt, va_list args);
b32    Str_Equal(String a, String b);
b32    Str_EqualNoCase(String a, String b);                  // ASCII letters fold

// Text built up piece by piece in an arena (grows like Array).
typedef struct TextBuilder { Arena *arena; u8 *data; s64 count, capacity; } TextBuilder;
void   TB_Append(TextBuilder *tb, String s);
void   TB_Format(TextBuilder *tb, const char *fmt, ...);
String TB_String(TextBuilder *tb);

// One codepoint at s[*at] (moves *at on; a bad byte is U+FFFD, one byte); a codepoint's bytes.
u32     Utf8_Decode(String s, s64 *at);
s64     Utf8_Encode(u8 out[4], u32 codepoint);
WString Utf8ToUtf16(Arena *arena, String s);                 // NUL-terminated
String  Utf16ToUtf8(Arena *arena, WString s);

u64 Hash_Bytes(const void *data, u64 size, u64 seed);
u64 Time_NowMicros(void);   // since the first call

static inline s32 RoundS32(f32 x) { return (s32)(x < 0 ? x - 0.5f : x + 0.5f); }
static inline f32 RoundF32(f32 x) { return (f32)RoundS32(x); }
static inline f32 LerpF32(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

#ifdef __cplusplus
}
#endif

#endif
