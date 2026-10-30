// DdsWic.dll: Windows Imaging Component decoder for every DDS format DirectXTex reads
// (BC1-BC7, sRGB, uncompressed, float, legacy). Windows' own DDS decoder only reads BC1-BC3.
// Install: regsvr32 DdsWic.dll (elevated). Uninstall: regsvr32 /u DdsWic.dll.
#include <windows.h>
#include <olectl.h>
#include <shlobj.h>
#include <wincodec.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "DirectXTex.h"

namespace {

// {CC010FCE-4965-4BEB-8DA4-DD0715AA7D07}
const CLSID CLSID_DdsDecoder = {0xcc010fce, 0x4965, 0x4beb, {0x8d, 0xa4, 0xdd, 0x07, 0x15, 0xaa, 0x7d, 0x07}};
// {B280DAF7-39D2-4D9B-95FA-29777A4B3651}
const GUID GUID_VendorDdsViewer = {0xb280daf7, 0x39d2, 0x4d9b, {0x95, 0xfa, 0x29, 0x77, 0x7a, 0x4b, 0x36, 0x51}};

const wchar_t kFriendlyName[] = L"DDS Decoder (DirectXTex, all formats)";

HMODULE g_module = nullptr;
std::atomic<long> g_objects{0};
std::atomic<long> g_locks{0};

// Minimal IUnknown implementation shared by the COM objects below.
template <class Derived>
class ComBase {
public:
    ComBase() { ++g_objects; }
    virtual ~ComBase() { --g_objects; }

protected:
    ULONG addRef() { return ++refs_; }
    ULONG release() {
        ULONG r = --refs_;
        if (r == 0) delete static_cast<Derived*>(this);
        return r;
    }

private:
    std::atomic<ULONG> refs_{1};
};

struct Bgra {
    UINT width = 0;
    UINT height = 0;
    std::vector<uint8_t> pixels;  // top-down, stride = width * 4
};

// Loaded file shared by the decoder and its frame. Mipmaps are converted to BGRA on first use.
// Only the first face / array item / depth slice is shown.
struct Texture {
    DirectX::ScratchImage scratch;
    DirectX::TexMetadata meta = {};
    std::mutex lock;
    std::vector<std::unique_ptr<Bgra>> mips;
    std::atomic<ULONG> refs{1};

    void addRef() { ++refs; }
    void release() {
        if (--refs == 0) delete this;
    }

    UINT mipWidth(size_t i) const { return UINT(std::max<size_t>(1, meta.width >> i)); }
    UINT mipHeight(size_t i) const { return UINT(std::max<size_t>(1, meta.height >> i)); }

    const Bgra* mip(size_t i) {
        std::lock_guard<std::mutex> guard(lock);
        if (i >= meta.mipLevels) return nullptr;
        if (mips.empty()) mips.resize(meta.mipLevels);
        if (!mips[i]) mips[i] = convert(i);
        return mips[i].get();
    }

private:
    std::unique_ptr<Bgra> convert(size_t i) {
        using namespace DirectX;
        const Image* img = scratch.GetImage(i, 0, 0);
        if (!img) return nullptr;

        // sRGB textures are shown as stored (no gamma change), like any sRGB image on screen.
        const DXGI_FORMAT target = IsSRGB(img->format) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;

        ScratchImage decompressed, converted;
        const Image* cur = img;
        if (IsCompressed(cur->format)) {
            if (FAILED(Decompress(*cur, DXGI_FORMAT_UNKNOWN, decompressed))) return nullptr;
            cur = decompressed.GetImage(0, 0, 0);
        }
        if (cur->format != target) {
            // DirectXTex's own converter: its WIC path needs COM set up by the host process.
            if (FAILED(Convert(*cur, target, TEX_FILTER_FORCE_NON_WIC, TEX_THRESHOLD_DEFAULT, converted))) return nullptr;
            cur = converted.GetImage(0, 0, 0);
        }

        auto out = std::make_unique<Bgra>();
        out->width = UINT(cur->width);
        out->height = UINT(cur->height);
        out->pixels.resize(size_t(out->width) * out->height * 4);
        for (UINT y = 0; y < out->height; ++y)
            std::memcpy(&out->pixels[size_t(y) * out->width * 4], cur->pixels + y * cur->rowPitch, size_t(out->width) * 4);
        return out;
    }
};

HRESULT readStream(IStream* stream, std::vector<uint8_t>& out) {
    if (!stream) return E_INVALIDARG;
    STATSTG st = {};
    HRESULT hr = stream->Stat(&st, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;
    if (st.cbSize.QuadPart < 128 || st.cbSize.QuadPart > (1ULL << 31)) return WINCODEC_ERR_BADIMAGE;
    LARGE_INTEGER zero = {};
    hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) return hr;
    out.resize(size_t(st.cbSize.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        ULONG got = 0;
        hr = stream->Read(out.data() + done, ULONG(out.size() - done), &got);
        if (FAILED(hr)) return hr;
        if (got == 0) break;
        done += got;
    }
    return done == out.size() ? S_OK : WINCODEC_ERR_BADIMAGE;
}

// Enfusion .edds files start with a DDS header followed by a COPY / LZ4 block table. They are
// left to an EDDS decoder: claiming them here would hide them from it.
bool isEdds(const std::vector<uint8_t>& d) {
    if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0) return false;
    const size_t at = std::memcmp(d.data() + 84, "DX10", 4) == 0 ? 148 : 128;
    return d.size() >= at + 4 && (std::memcmp(&d[at], "COPY", 4) == 0 || std::memcmp(&d[at], "LZ4 ", 4) == 0);
}

// Formats DirectXTex can load but not turn into BGRA (video / planar / typeless depth).
bool displayable(const DirectX::TexMetadata& meta) {
    using namespace DirectX;
    return !IsVideo(meta.format) && !IsPlanar(meta.format) && !IsTypeless(meta.format, false) &&
           !IsDepthStencil(meta.format) && meta.width > 0 && meta.height > 0;
}

HRESULT copyRect(const Bgra& img, const WICRect* prc, UINT stride, UINT bufferSize, BYTE* buffer) {
    WICRect full = {0, 0, INT(img.width), INT(img.height)};
    const WICRect& r = prc ? *prc : full;
    if (r.X < 0 || r.Y < 0 || r.Width < 0 || r.Height < 0 || UINT(r.X + r.Width) > img.width ||
        UINT(r.Y + r.Height) > img.height)
        return E_INVALIDARG;
    const UINT rowBytes = UINT(r.Width) * 4;
    if (stride < rowBytes) return E_INVALIDARG;
    if (r.Height > 0 && UINT64(stride) * (r.Height - 1) + rowBytes > bufferSize) return WINCODEC_ERR_INSUFFICIENTBUFFER;
    for (INT y = 0; y < r.Height; ++y)
        std::memcpy(buffer + size_t(stride) * y, &img.pixels[(size_t(r.Y + y) * img.width + r.X) * 4], rowBytes);
    return S_OK;
}

class Frame : public ComBase<Frame>, public IWICBitmapFrameDecode, public IWICBitmapSourceTransform {
public:
    explicit Frame(Texture* tex) : tex_(tex) { tex_->addRef(); }
    ~Frame() override { tex_->release(); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IWICBitmapSource || iid == IID_IWICBitmapFrameDecode)
            *out = static_cast<IWICBitmapFrameDecode*>(this);
        else if (iid == IID_IWICBitmapSourceTransform)
            *out = static_cast<IWICBitmapSourceTransform*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        addRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return addRef(); }
    STDMETHODIMP_(ULONG) Release() override { return release(); }

    // IWICBitmapSource
    STDMETHODIMP GetSize(UINT* w, UINT* h) override {
        if (!w || !h) return E_INVALIDARG;
        *w = tex_->mipWidth(0);
        *h = tex_->mipHeight(0);
        return S_OK;
    }
    STDMETHODIMP GetPixelFormat(WICPixelFormatGUID* fmt) override {
        if (!fmt) return E_INVALIDARG;
        *fmt = GUID_WICPixelFormat32bppBGRA;
        return S_OK;
    }
    STDMETHODIMP GetResolution(double* x, double* y) override {
        if (!x || !y) return E_INVALIDARG;
        *x = *y = 96.0;
        return S_OK;
    }
    STDMETHODIMP CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    STDMETHODIMP CopyPixels(const WICRect* prc, UINT stride, UINT size, BYTE* buffer) override {
        if (!buffer) return E_INVALIDARG;
        const Bgra* img = tex_->mip(0);
        return img ? copyRect(*img, prc, stride, size, buffer) : WINCODEC_ERR_BADIMAGE;
    }

    // IWICBitmapFrameDecode
    STDMETHODIMP GetMetadataQueryReader(IWICMetadataQueryReader**) override {
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetColorContexts(UINT, IWICColorContext**, UINT* actual) override {
        if (!actual) return E_INVALIDARG;
        *actual = 0;
        return S_OK;
    }
    STDMETHODIMP GetThumbnail(IWICBitmapSource**) override { return WINCODEC_ERR_CODECNOTHUMBNAIL; }

    // IWICBitmapSourceTransform: lets thumbnailers read a small mipmap instead of the full texture.
    STDMETHODIMP CopyPixels(const WICRect* prc, UINT w, UINT h, WICPixelFormatGUID* fmt,
                            WICBitmapTransformOptions transform, UINT stride, UINT size, BYTE* buffer) override {
        if (!buffer) return E_INVALIDARG;
        if (transform != WICBitmapTransformRotate0) return WINCODEC_ERR_UNSUPPORTEDOPERATION;
        if (fmt && *fmt != GUID_WICPixelFormat32bppBGRA) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        for (size_t i = 0; i < tex_->meta.mipLevels; ++i) {
            if (tex_->mipWidth(i) == w && tex_->mipHeight(i) == h) {
                const Bgra* img = tex_->mip(i);
                return img ? copyRect(*img, prc, stride, size, buffer) : WINCODEC_ERR_BADIMAGE;
            }
        }
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetClosestSize(UINT* w, UINT* h) override {
        if (!w || !h) return E_INVALIDARG;
        // Smallest mipmap that still covers the request; the full size otherwise.
        size_t best = 0;
        for (size_t i = 0; i < tex_->meta.mipLevels; ++i)
            if (tex_->mipWidth(i) >= *w && tex_->mipHeight(i) >= *h) best = i;
        *w = tex_->mipWidth(best);
        *h = tex_->mipHeight(best);
        return S_OK;
    }
    STDMETHODIMP GetClosestPixelFormat(WICPixelFormatGUID* fmt) override {
        if (!fmt) return E_INVALIDARG;
        *fmt = GUID_WICPixelFormat32bppBGRA;
        return S_OK;
    }
    STDMETHODIMP DoesSupportTransform(WICBitmapTransformOptions transform, BOOL* supported) override {
        if (!supported) return E_INVALIDARG;
        *supported = transform == WICBitmapTransformRotate0;
        return S_OK;
    }

private:
    Texture* tex_;
};

class Decoder : public ComBase<Decoder>, public IWICBitmapDecoder {
public:
    ~Decoder() override {
        if (tex_) tex_->release();
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IWICBitmapDecoder) {
            *out = static_cast<IWICBitmapDecoder*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return addRef(); }
    STDMETHODIMP_(ULONG) Release() override { return release(); }

    // IWICBitmapDecoder
    STDMETHODIMP QueryCapability(IStream* stream, DWORD* caps) override {
        if (!caps) return E_INVALIDARG;
        *caps = 0;
        std::vector<uint8_t> data;
        HRESULT hr = readStream(stream, data);
        if (FAILED(hr)) return hr;
        DirectX::TexMetadata meta;
        if (!isEdds(data) &&
            SUCCEEDED(DirectX::GetMetadataFromDDSMemory(data.data(), data.size(), DirectX::DDS_FLAGS_NONE, meta)) &&
            displayable(meta))
            *caps = WICBitmapDecoderCapabilityCanDecodeAllImages | WICBitmapDecoderCapabilityCanDecodeSomeImages;
        return S_OK;
    }
    STDMETHODIMP Initialize(IStream* stream, WICDecodeOptions) override {
        if (tex_) return WINCODEC_ERR_WRONGSTATE;
        std::vector<uint8_t> data;
        HRESULT hr = readStream(stream, data);
        if (FAILED(hr)) return hr;
        if (isEdds(data)) return WINCODEC_ERR_BADHEADER;
        Texture* tex = new (std::nothrow) Texture;
        if (!tex) return E_OUTOFMEMORY;
        hr = DirectX::LoadFromDDSMemory(data.data(), data.size(), DirectX::DDS_FLAGS_PERMISSIVE, &tex->meta,
                                        tex->scratch);
        if (SUCCEEDED(hr) && !displayable(tex->meta)) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        if (FAILED(hr)) {
            tex->release();
            return hr == E_INVALIDARG || hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) ? WINCODEC_ERR_BADHEADER : hr;
        }
        tex_ = tex;
        return S_OK;
    }
    STDMETHODIMP GetContainerFormat(GUID* fmt) override {
        if (!fmt) return E_INVALIDARG;
        *fmt = GUID_ContainerFormatDds;
        return S_OK;
    }
    STDMETHODIMP GetDecoderInfo(IWICBitmapDecoderInfo** info) override {
        if (!info) return E_INVALIDARG;
        *info = nullptr;
        IWICImagingFactory* factory = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;
        IWICComponentInfo* component = nullptr;
        hr = factory->CreateComponentInfo(CLSID_DdsDecoder, &component);
        if (SUCCEEDED(hr)) {
            hr = component->QueryInterface(IID_PPV_ARGS(info));
            component->Release();
        }
        factory->Release();
        return hr;
    }
    STDMETHODIMP CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    STDMETHODIMP GetMetadataQueryReader(IWICMetadataQueryReader**) override {
        return WINCODEC_ERR_UNSUPPORTEDOPERATION;
    }
    STDMETHODIMP GetPreview(IWICBitmapSource**) override { return WINCODEC_ERR_UNSUPPORTEDOPERATION; }
    STDMETHODIMP GetColorContexts(UINT, IWICColorContext**, UINT* actual) override {
        if (!actual) return E_INVALIDARG;
        *actual = 0;
        return S_OK;
    }
    STDMETHODIMP GetThumbnail(IWICBitmapSource**) override { return WINCODEC_ERR_CODECNOTHUMBNAIL; }
    STDMETHODIMP GetFrameCount(UINT* count) override {
        if (!count) return E_INVALIDARG;
        *count = tex_ ? 1 : 0;
        return tex_ ? S_OK : WINCODEC_ERR_NOTINITIALIZED;
    }
    STDMETHODIMP GetFrame(UINT index, IWICBitmapFrameDecode** frame) override {
        if (!frame) return E_INVALIDARG;
        *frame = nullptr;
        if (!tex_) return WINCODEC_ERR_NOTINITIALIZED;
        if (index != 0) return WINCODEC_ERR_FRAMEMISSING;
        Frame* f = new (std::nothrow) Frame(tex_);
        if (!f) return E_OUTOFMEMORY;
        *frame = f;
        return S_OK;
    }

private:
    Texture* tex_ = nullptr;
};

class ClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *out = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }  // static instance
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Decoder* d = new (std::nothrow) Decoder;
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->QueryInterface(iid, out);
        d->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_locks;
        else --g_locks;
        return S_OK;
    }
};

ClassFactory g_factory;

// ---- Registration -------------------------------------------------------------------------

std::wstring guidString(const GUID& g) {
    wchar_t buf[40];
    StringFromGUID2(g, buf, 40);
    return buf;
}

bool setValue(const std::wstring& path, const wchar_t* name, DWORD type, const void* data, DWORD size) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(data), size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool setString(const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    return setValue(path, name, REG_SZ, value.c_str(), DWORD((value.size() + 1) * sizeof(wchar_t)));
}

bool setDword(const std::wstring& path, const wchar_t* name, DWORD value) {
    return setValue(path, name, REG_DWORD, &value, sizeof(value));
}

const wchar_t kClasses[] = L"SOFTWARE\\Classes\\";

HRESULT registerServer() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return SELFREG_E_CLASS;

    const std::wstring clsid = guidString(CLSID_DdsDecoder);
    const std::wstring cls = std::wstring(kClasses) + L"CLSID\\" + clsid;
    bool ok = true;

    ok &= setString(cls, nullptr, kFriendlyName);
    ok &= setString(cls, L"FriendlyName", kFriendlyName);
    ok &= setString(cls, L"Author", L"dds-viewer");
    ok &= setString(cls, L"Version", L"1.0.0.0");
    ok &= setString(cls, L"SpecVersion", L"1.0.0.0");
    ok &= setString(cls, L"Vendor", guidString(GUID_VendorDdsViewer));
    ok &= setString(cls, L"ContainerFormat", guidString(GUID_ContainerFormatDds));
    ok &= setString(cls, L"FileExtensions", L".dds");
    ok &= setString(cls, L"MimeTypes", L"image/vnd.ms-dds");
    ok &= setDword(cls, L"SupportAnimation", 0);
    ok &= setDword(cls, L"SupportChromakey", 0);
    ok &= setDword(cls, L"SupportLossless", 1);
    ok &= setDword(cls, L"SupportMultiframe", 0);
    ok &= setDword(cls, L"ArbitrationPriority", 10);
    ok &= setString(cls + L"\\InprocServer32", nullptr, path);
    ok &= setString(cls + L"\\InprocServer32", L"ThreadingModel", L"Both");
    ok &= setString(cls + L"\\Formats\\" + guidString(GUID_WICPixelFormat32bppBGRA), nullptr, L"");

    // Content sniffing: "DDS " magic.
    static const BYTE magic[4] = {'D', 'D', 'S', ' '};
    static const BYTE mask[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    const std::wstring pat = cls + L"\\Patterns\\0";
    ok &= setDword(pat, L"Position", 0);
    ok &= setDword(pat, L"Length", 4);
    ok &= setValue(pat, L"Pattern", REG_BINARY, magic, 4);
    ok &= setValue(pat, L"Mask", REG_BINARY, mask, 4);

    // WIC decoder category.
    const std::wstring inst =
        std::wstring(kClasses) + L"CLSID\\" + guidString(CATID_WICBitmapDecoders) + L"\\Instance\\" + clsid;
    ok &= setString(inst, L"CLSID", clsid);
    ok &= setString(inst, L"FriendlyName", kFriendlyName);

    // .dds is already wired to the system photo thumbnail and preview handlers by Windows.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok ? S_OK : SELFREG_E_CLASS;
}

HRESULT unregisterServer() {
    const std::wstring clsid = guidString(CLSID_DdsDecoder);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + clsid).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + guidString(CATID_WICBitmapDecoders) +
                                        L"\\Instance\\" + clsid).c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (clsid != CLSID_DdsDecoder) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(iid, out);
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() { return registerServer(); }

STDAPI DllUnregisterServer() { return unregisterServer(); }
