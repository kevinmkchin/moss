// term: folder listings for the browser. The platform lists folders on a worker thread, watches the shown ones and
// measures their subfolders' sizes in the background (term_files_win32.c); the app gets whole listings, "this
// folder changed" notes and folder sizes in its frame input.
#ifndef TERM_FILES_H
#define TERM_FILES_H

#include "term_core.h"

// File_Sized: a folder whose size (all under it) is known (the app's). File_Cloud: an online-only placeholder (OneDrive and
// the like): opening it downloads it, so nothing does unless the user asks.
enum { File_Dir = 1, File_Hidden = 2, File_Drive = 4, File_Link = 8, File_Sized = 16, File_Cloud = 32 };

typedef struct FileEntry {
    String name;         // a drive: "C:\"
    String label;        // a drive's volume name; else empty
    u64 size;            // a drive: its total bytes
    u64 free;            // a drive: free bytes
    u64 modified;        // FILETIME
    u32 flags;           // File_*
} FileEntry;

typedef struct FileListing {
    u32 request;         // what asked for it (FilesOut.request)
    String path;         // "" = This PC (the drives)
    s32 error;           // a Win32 error (5: access denied), 0 = fine
    s64 count;
    FileEntry *entries;
} FileListing;

// A subfolder's size: everything under it (links not followed).
typedef struct FolderSize {
    String folder;       // the watched folder it's in (as watched)
    String name;         // the subfolder's name
    u64 bytes, files, folders;
} FolderSize;

// To the platform: list this folder; watch these (sizes: measure their subfolders too, on local SSDs).
typedef struct FileRequest { u32 request; String path; } FileRequest;
typedef struct FileWatch { String path; b32 sizes; } FileWatch;

// Listings, changes and sizes since the last frame (frame memory).
typedef struct FilesIn {
    s64 listing_count;
    FileListing *listings;
    s64 changed_count;
    String *changed;     // folders whose contents changed (watched ones)
    s64 size_count;
    FolderSize *sizes;
} FilesIn;

void Files_Start(void *wake_window, u32 wake_message);   // posts wake_message to the window when results wait
void Files_Request(const FileRequest *r);
void Files_Watch(const FileWatch *watches, s64 count);    // exactly these (the others stop)
void Files_Drain(FilesIn *in, Arena *frame_arena);
void Files_Stop(void);

#endif
