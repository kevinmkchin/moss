// term: the C++ unity build: the server (from clod: emulator, ConPTY, tabs, claude and agent detection,
// tray), the window's server client and terminal cell renderer, images, the UI fonts and the GPU.
// The C side (term_c.c) calls into it through term_platform.h.

#define _CRT_RAND_S
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <d2d1.h>
#include <dwrite_2.h>
#include <dcomp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include <algorithm>
#include <map>

extern "C" {
#include "term_core.h"
#include "term_types.h"
#include "term_app.h"
#include "term_platform.h"
#include "term_debug.h"
}
#undef S // term_core.h's string literal macro; the C++ side names variables S
#include "term_base.h"

static_assert(sizeof(cell) == sizeof(TermCell), "TermCell must match the emulator's cell");

#include "term_ipc.cpp"
#include "term_sessions.cpp"
#include "term_vt.cpp"
#include "term_pty.cpp"
#include "term_settings.cpp"
#include "term_image.cpp"
#include "term_server.cpp"
#include "term_client.cpp"
#include "term_cells.cpp"
#include "term_font.cpp"
#include "term_gpu.cpp"

// --- small services for the C side -------------------------------------------------------------------------

extern "C" int Term_ServerMain(void) { return ServerMain(); }

extern "C" void Term_PrepareUser(void)
{
    SeedUserSetup();
}

extern "C" String Term_ReadSetting(Arena* A, const char* Key)
{
    std::string V = ReadSetting(Key);
    return Str_PushCopy(A, String{(u8*)V.data(), (s64)V.size()});
}

extern "C" void Term_WriteSetting(const char* Key, String Value)
{
    WriteSetting(Key, std::string((const char*)Value.data, (size_t)Value.count));
}

extern "C" void Term_ResetSettings(void) { ResetSettings(); }
extern "C" b32 Term_GoodProgrammer(void) { return GoodProgrammerOn(); }
extern "C" b32 Term_SetGoodProgrammer(b32 On) { return SetGoodProgrammer(On); }

extern "C" String Term_ReadFile(Arena* A, const wchar_t* Path)
{
    std::string V = ReadWholeFile(Path);
    return Str_PushCopy(A, String{(u8*)V.data(), (s64)V.size()});
}

extern "C" void Term_WriteFileAtomic(const wchar_t* Path, String Data)
{
    std::wstring Temp = std::wstring(Path) + L".tmp";
    HANDLE File = CreateFileW(Temp.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { Logf("file", "can't write %s", WideToUtf8(Temp).c_str()); return; }
    DWORD Written = 0;
    WriteFile(File, Data.data, (DWORD)Data.count, &Written, 0);
    CloseHandle(File);
    if(!MoveFileExW(Temp.c_str(), Path, MOVEFILE_REPLACE_EXISTING)) { Logf("file", "can't replace %s (error %lu)", WideToUtf8(Path).c_str(), GetLastError()); }
}

extern "C" void Term_RaiseForTyping(void) { RaiseForTyping(GetCurrentProcess()); }
extern "C" void Term_LatencyMark(const char* What) { LatencyMark(What); }

// --- `term --vt-replay FILE`: feed a recorded byte stream to the emulator, print the screen ---------------
extern "C" int Term_VtReplayMain(const wchar_t* File, int Cols, int Rows)
{
    std::string Bytes = ReadWholeFile(File);
    if(Bytes.empty()) { Logf("replay", "nothing to replay in %s", WideToUtf8(File).c_str()); return 1; }
    static vt_terminal T;
    VtInit(T, Max(Cols, 2), Max(Rows, 2));
    VtFeed(T, (const u8*)Bytes.data(), Bytes.size());
    std::string Out;
    for(s32 Y = 0; Y < T.Rows; ++Y)
    {
        std::string Line;
        size_t Ink = 0;
        for(s32 X = 0; X < T.Cols; ++X)
        {
            const cell& K = T.Screens[T.AltActive ? 1 : 0][(size_t)Y * T.Cols + X];
            if(K.Flags & CELL_WIDE_CONT) { continue; }
            AppendUtf8(Line, K.Codepoint >= 0x20 ? K.Codepoint : ' ');
            if(K.Codepoint > ' ') { Ink = Line.size(); }
        }
        Line.resize(Ink);
        Out += Line + "\n";
    }
    Logf("replay", "%s: %d bytes, %dx%d, cursor %d,%d, %d scrollback lines, title \"%s\"", WideToUtf8(File).c_str(), (int)Bytes.size(), T.Cols, T.Rows,
         T.CursorX, T.CursorY, (int)T.Scrollback.size(), T.Title.c_str());
    HANDLE StdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD Written = 0;
    if(StdOut && StdOut != INVALID_HANDLE_VALUE) { WriteFile(StdOut, Out.data(), (DWORD)Out.size(), &Written, 0); }
    return 0;
}

// --- images (WIC) ------------------------------------------------------------------------------------------
extern "C" b32 Image_SavePng(const wchar_t* Path, const u8* Rgba, s32 W, s32 H)
{
    IWICImagingFactory* Wic = 0;
    IWICStream* Stream = 0;
    IWICBitmapEncoder* Encoder = 0;
    IWICBitmapFrameEncode* Frame = 0;
    // RGBA rows to BGRA (what the PNG encoder takes everywhere)
    std::vector<u8> Bgra((size_t)W * H * 4);
    for(size_t I = 0; I < Bgra.size(); I += 4) { Bgra[I] = Rgba[I + 2]; Bgra[I + 1] = Rgba[I + 1]; Bgra[I + 2] = Rgba[I]; Bgra[I + 3] = Rgba[I + 3]; }
    HRESULT Hr = CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Wic));
    const char* Step = "WIC factory";
    if(SUCCEEDED(Hr)) { Step = "stream"; Hr = Wic->CreateStream(&Stream); }
    if(SUCCEEDED(Hr)) { Step = "file"; Hr = Stream->InitializeFromFilename(Path, GENERIC_WRITE); }
    if(SUCCEEDED(Hr)) { Step = "encoder"; Hr = Wic->CreateEncoder(GUID_ContainerFormatPng, 0, &Encoder); }
    if(SUCCEEDED(Hr)) { Hr = Encoder->Initialize(Stream, WICBitmapEncoderNoCache); }
    if(SUCCEEDED(Hr)) { Step = "frame"; Hr = Encoder->CreateNewFrame(&Frame, 0); }
    if(SUCCEEDED(Hr)) { Hr = Frame->Initialize(0); }
    if(SUCCEEDED(Hr)) { Hr = Frame->SetSize((UINT)W, (UINT)H); }
    WICPixelFormatGUID Format = GUID_WICPixelFormat32bppBGRA;
    if(SUCCEEDED(Hr)) { Step = "pixel format"; Hr = Frame->SetPixelFormat(&Format); }
    if(SUCCEEDED(Hr) && !IsEqualGUID(Format, GUID_WICPixelFormat32bppBGRA)) { Hr = E_FAIL; }
    if(SUCCEEDED(Hr)) { Step = "pixels"; Hr = Frame->WritePixels((UINT)H, (UINT)W * 4, (UINT)(W * H * 4), Bgra.data()); }
    if(SUCCEEDED(Hr)) { Step = "commit"; Hr = Frame->Commit(); }
    if(SUCCEEDED(Hr)) { Hr = Encoder->Commit(); }
    bool32 Ok = SUCCEEDED(Hr);
    if(!Ok) { LogHr("image", Step, Hr); }
    if(Frame) { Frame->Release(); }
    if(Encoder) { Encoder->Release(); }
    if(Stream) { Stream->Release(); }
    if(Wic) { Wic->Release(); }
    if(!Ok) { Logf("image", "couldn't write %s", WideToUtf8(Path).c_str()); }
    return Ok;
}

extern "C" u8* Image_LoadPng(const wchar_t* Path, Arena* A, s32* W, s32* H)
{
    IWICImagingFactory* Wic = 0;
    IWICBitmapDecoder* Decoder = 0;
    IWICBitmapFrameDecode* Frame = 0;
    IWICFormatConverter* Converter = 0;
    u8* Out = 0;
    UINT IW = 0, IH = 0;
    bool32 Ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Wic)));
    Ok = Ok && SUCCEEDED(Wic->CreateDecoderFromFilename(Path, 0, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &Decoder));
    Ok = Ok && SUCCEEDED(Decoder->GetFrame(0, &Frame));
    Ok = Ok && SUCCEEDED(Wic->CreateFormatConverter(&Converter));
    Ok = Ok && SUCCEEDED(Converter->Initialize(Frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, 0, 0.0, WICBitmapPaletteTypeCustom));
    Ok = Ok && SUCCEEDED(Converter->GetSize(&IW, &IH)) && IW && IH;
    if(Ok)
    {
        Out = (u8*)Arena_Push(A, (u64)IW * IH * 4, 16);
        Ok = SUCCEEDED(Converter->CopyPixels(0, IW * 4, IW * IH * 4, Out));
        *W = (s32)IW;
        *H = (s32)IH;
    }
    if(Converter) { Converter->Release(); }
    if(Frame) { Frame->Release(); }
    if(Decoder) { Decoder->Release(); }
    if(Wic) { Wic->Release(); }
    return Ok ? Out : 0;
}

extern "C" u8* Image_Decode(const wchar_t* Path, s32 MaxW, s32 MaxH, s32* W, s32* H, s32* FullW, s32* FullH)
{
    std::vector<u32> Pixels;
    u32 PW = 0, PH = 0, FW = 0, FH = 0;
    if(!DecodeImage(Path, (u32)MaxW, (u32)MaxH, Pixels, &PW, &PH, &FW, &FH)) { return 0; }
    u8* Out = (u8*)malloc(Pixels.size() * 4);
    if(!Out) { return 0; }
    for(size_t I = 0; I < Pixels.size(); I++)
    {
        // premultiplied BGRA words (0xAARRGGBB) -> RGBA bytes
        u32 P = Pixels[I];
        Out[I * 4 + 0] = (u8)(P >> 16);
        Out[I * 4 + 1] = (u8)(P >> 8);
        Out[I * 4 + 2] = (u8)P;
        Out[I * 4 + 3] = (u8)(P >> 24);
    }
    *W = (s32)PW;
    *H = (s32)PH;
    *FullW = (s32)FW;
    *FullH = (s32)FH;
    return Out;
}
