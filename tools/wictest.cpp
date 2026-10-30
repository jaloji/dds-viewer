// wictest: decodes a DDS through WIC and saves it as PNG with WIC's own encoder.
// Usage: wictest <DdsWic.dll|--system> <in.dds> <out.png> [--thumb N]
//   DdsWic.dll  loads the codec directly (no registration needed)
//   --system    goes through the registered codecs, like Explorer does, and prints which one
//   --thumb N   asks for an N x N source through IWICBitmapSourceTransform (thumbnail path)
#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(expr)                                                         \
    do {                                                                    \
        HRESULT hr_ = (expr);                                               \
        if (FAILED(hr_)) {                                                  \
            std::fprintf(stderr, "%s failed: 0x%08lX\n", #expr, (unsigned long)hr_); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static std::wstring widen(const char* s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    w.resize(size_t(n - 1));
    return w;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: wictest <DdsWic.dll|--system> <in.dds> <out.png> [--thumb N]\n");
        return 2;
    }
    UINT thumb = 0;
    if (argc >= 6 && !std::strcmp(argv[4], "--thumb")) thumb = UINT(std::atoi(argv[5]));

    CHECK(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IWICImagingFactory* factory = nullptr;
    CHECK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));

    IWICBitmapDecoder* decoder = nullptr;
    const std::wstring in = widen(argv[2]);
    if (!std::strcmp(argv[1], "--system")) {
        CHECK(factory->CreateDecoderFromFilename(in.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                 &decoder));
    } else {
        HMODULE dll = LoadLibraryW(widen(argv[1]).c_str());
        if (!dll) {
            std::fprintf(stderr, "cannot load %s (error %lu)\n", argv[1], GetLastError());
            return 1;
        }
        auto getClass = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
            GetProcAddress(dll, "DllGetClassObject"));
        // {CC010FCE-4965-4BEB-8DA4-DD0715AA7D07}
        const CLSID clsid = {0xcc010fce, 0x4965, 0x4beb, {0x8d, 0xa4, 0xdd, 0x07, 0x15, 0xaa, 0x7d, 0x07}};
        IClassFactory* cf = nullptr;
        CHECK(getClass(clsid, IID_PPV_ARGS(&cf)));
        CHECK(cf->CreateInstance(nullptr, IID_PPV_ARGS(&decoder)));
        IWICStream* stream = nullptr;
        CHECK(factory->CreateStream(&stream));
        CHECK(stream->InitializeFromFilename(in.c_str(), GENERIC_READ));
        DWORD caps = 0;
        CHECK(decoder->QueryCapability(stream, &caps));
        std::printf("QueryCapability: 0x%lX\n", caps);
        CHECK(decoder->Initialize(stream, WICDecodeMetadataCacheOnDemand));
    }

    IWICBitmapDecoderInfo* info = nullptr;
    if (SUCCEEDED(decoder->GetDecoderInfo(&info))) {
        wchar_t name[256] = {};
        UINT len = 0;
        info->GetFriendlyName(256, name, &len);
        std::printf("decoder: %ls\n", name);
        info->Release();
    }
    IWICBitmapFrameDecode* frame = nullptr;
    CHECK(decoder->GetFrame(0, &frame));
    UINT w = 0, h = 0;
    CHECK(frame->GetSize(&w, &h));
    std::printf("frame: %ux%u\n", w, h);

    std::vector<BYTE> pixels;
    if (thumb) {
        IWICBitmapSourceTransform* tr = nullptr;
        CHECK(frame->QueryInterface(IID_PPV_ARGS(&tr)));
        w = h = thumb;
        CHECK(tr->GetClosestSize(&w, &h));
        std::printf("closest size for %u: %ux%u\n", thumb, w, h);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        pixels.resize(size_t(w) * h * 4);
        CHECK(tr->CopyPixels(nullptr, w, h, &fmt, WICBitmapTransformRotate0, w * 4, UINT(pixels.size()), pixels.data()));
        tr->Release();
    } else {
        // Other decoders (Windows' own DDS one) may use another pixel format.
        IWICFormatConverter* conv = nullptr;
        CHECK(factory->CreateFormatConverter(&conv));
        CHECK(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom));
        pixels.resize(size_t(w) * h * 4);
        CHECK(conv->CopyPixels(nullptr, w * 4, UINT(pixels.size()), pixels.data()));
        conv->Release();
    }

    IWICStream* out = nullptr;
    CHECK(factory->CreateStream(&out));
    CHECK(out->InitializeFromFilename(widen(argv[3]).c_str(), GENERIC_WRITE));
    IWICBitmapEncoder* enc = nullptr;
    CHECK(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc));
    CHECK(enc->Initialize(out, WICBitmapEncoderNoCache));
    IWICBitmapFrameEncode* ef = nullptr;
    CHECK(enc->CreateNewFrame(&ef, nullptr));
    CHECK(ef->Initialize(nullptr));
    CHECK(ef->SetSize(w, h));
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    CHECK(ef->SetPixelFormat(&pf));
    CHECK(ef->WritePixels(h, w * 4, UINT(pixels.size()), pixels.data()));
    CHECK(ef->Commit());
    CHECK(enc->Commit());
    std::printf("wrote %s\n", argv[3]);

    ef->Release();
    enc->Release();
    out->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    CoUninitialize();
    return 0;
}
