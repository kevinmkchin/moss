// Writes moss's icon as a multi-size .ico. The icon is a 1024 master PNG (tools/icon/moss-icon-1024.png):
// the Moss sprout cut out of a photo of moss (Wikimedia Commons, "Moss high resolution.jpg", public domain),
// its texture toned from deep green to lime, on a paper block. Each size is scaled down from the master by WIC.
// build.bat runs this and embeds the result in moss.exe, which is where Explorer, the taskbar,
// the console window and the tray all get the icon from.
//
//   make_icon <output.ico> <moss-icon-1024.png>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>

#include "../code/term_base.h"

// Renders at Size x Size into straight-alpha BGRA, top row first.
static bool ScaleIcon(IWICImagingFactory* Wic, IWICBitmapFrameDecode* Frame, s32 Size, std::vector<u32>& Out)
{
    IWICBitmapScaler* Scaler = 0;
    IWICFormatConverter* Convert = 0;
    bool Ok = false;
    if(SUCCEEDED(Wic->CreateBitmapScaler(&Scaler)) && SUCCEEDED(Scaler->Initialize(Frame, Size, Size, WICBitmapInterpolationModeHighQualityCubic)) &&
       SUCCEEDED(Wic->CreateFormatConverter(&Convert)) && SUCCEEDED(Convert->Initialize(Scaler, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, 0, 0, WICBitmapPaletteTypeCustom)))
    {
        Out.resize((size_t)Size * Size);
        Ok = SUCCEEDED(Convert->CopyPixels(0, Size * 4, Size * Size * 4, (BYTE*)Out.data()));
    }
    if(Convert) { Convert->Release(); }
    if(Scaler) { Scaler->Release(); }
    return Ok;
}

static void Put16(std::string& S, u32 V) { S += (char)(V & 0xFF); S += (char)((V >> 8) & 0xFF); }
static void Put32(std::string& S, u32 V) { Put16(S, V & 0xFFFF); Put16(S, V >> 16); }

int wmain(int Argc, wchar_t** Argv)
{
    if(Argc < 3) { fprintf(stderr, "usage: make_icon <output.ico> <moss-icon-1024.png>\n"); return 1; }
    static const s32 Sizes[] = {16, 20, 24, 32, 40, 48, 64, 256};
    const u32 Count = ArrayCount(Sizes);

    CoInitializeEx(0, COINIT_MULTITHREADED);
    IWICImagingFactory* Wic = 0;
    IWICBitmapDecoder* Decoder = 0;
    IWICBitmapFrameDecode* Frame = 0;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Wic))) ||
       FAILED(Wic->CreateDecoderFromFilename(Argv[2], 0, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &Decoder)) ||
       FAILED(Decoder->GetFrame(0, &Frame)))
    {
        fprintf(stderr, "make_icon: can't read the master icon\n");
        return 1;
    }

    // --- each image: BITMAPINFOHEADER (double height = colour + mask), BGRA bottom-up, empty AND mask ---
    std::vector<std::string> Images;
    for(u32 I = 0; I < Count; ++I)
    {
        s32 S = Sizes[I];
        std::vector<u32> Px;
        if(!ScaleIcon(Wic, Frame, S, Px)) { fprintf(stderr, "make_icon: can't scale to %d\n", S); return 1; }
        std::string Img;
        Put32(Img, 40);
        Put32(Img, S);
        Put32(Img, S * 2);
        Put16(Img, 1);
        Put16(Img, 32);
        Put32(Img, 0);
        Put32(Img, 0);
        Put32(Img, 0);
        Put32(Img, 0);
        Put32(Img, 0);
        Put32(Img, 0);
        for(s32 Y = S - 1; Y >= 0; --Y)
        {
            for(s32 X = 0; X < S; ++X) { Put32(Img, Px[(size_t)Y * S + X]); }
        }
        s32 MaskRow = ((S + 31) / 32) * 4;
        Img.append((size_t)MaskRow * S, '\0'); // alpha decides transparency; the mask is unused
        Images.push_back(Img);
    }
    Frame->Release();
    Decoder->Release();
    Wic->Release();
    CoUninitialize();

    // --- ICONDIR, one ICONDIRENTRY per image, then the images ---
    std::string File;
    Put16(File, 0);
    Put16(File, 1);
    Put16(File, Count);
    u32 Offset = 6 + 16 * Count;
    for(u32 I = 0; I < Count; ++I)
    {
        s32 S = Sizes[I];
        File += (char)(S >= 256 ? 0 : S);
        File += (char)(S >= 256 ? 0 : S);
        File += (char)0;
        File += (char)0;
        Put16(File, 1);
        Put16(File, 32);
        Put32(File, (u32)Images[I].size());
        Put32(File, Offset);
        Offset += (u32)Images[I].size();
    }
    for(const std::string& Img : Images) { File += Img; }

    FILE* Out = 0;
    _wfopen_s(&Out, Argv[1], L"wb");
    if(!Out) { fprintf(stderr, "make_icon: can't write output\n"); return 1; }
    fwrite(File.data(), 1, File.size(), Out);
    fclose(Out);
    return 0;
}
