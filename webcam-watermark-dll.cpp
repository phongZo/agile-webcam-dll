#include "pch.h"
#include <windows.h>
#include <string>
#include <atomic>
#include <mutex>
#include <map>
#include <dshow.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <gdiplus.h>
#include "DebugLog.h"
#include "../packages/minhook.1.3.3/lib/native/include/MinHook.h"
#include <nlohmann/json.hpp>
#include "Renderer/RendererManager.h"
#include "Renderer/RenderSnapshot.h"
#include <vector>
#include <memory>
#include <shlwapi.h>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")

// Helper to free AM_MEDIA_TYPE
void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0) {
        CoTaskMemFree((PVOID)mt.pbFormat);
        mt.cbFormat = 0;
        mt.pbFormat = NULL;
    }
    if (mt.pUnk != NULL) {
        mt.pUnk->Release();
        mt.pUnk = NULL;
    }
}

// Ensure NV12 subtype is defined if not already
#ifndef MEDIASUBTYPE_NV12
DEFINE_GUID(MEDIASUBTYPE_NV12, 0x3231564e, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif

using json = nlohmann::json;

#ifdef _WIN64
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x64-v141-mt.lib")
#else
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x86-v141-mt.lib")
#endif

// --- Global State ---
static HINSTANCE g_hModule = NULL;
static std::atomic<bool> g_bUnloading(false);
static std::atomic<bool> g_bInitialized(false);
static std::atomic<int> g_activeCalls(0); 
static std::mutex g_hookMutex;
static std::mutex g_drawMutex;
static ULONG_PTR g_gdiplusToken = 0;

static std::atomic<bool> g_bDShowHooked(false);
static std::atomic<bool> g_bMFHooked(false);
static std::atomic<bool> g_bMFCallbackHooked(false);
static bool g_isMirrorMode = false;

struct VideoConfig {
    int width = 0;
    int height = 0;
    int stride = 0;
    bool isNV12 = false;
};
static std::mutex g_cfgMutex;
static std::map<void*, VideoConfig> g_videoConfigs;

static HANDLE g_hIpcThread = NULL;
static const wchar_t* kPipeInject = L"\\\\.\\pipe\\AgileMarkPipe_qaKOab5VPyK4ar4A6sfm2VZ0";

static Gdiplus::Bitmap* g_pWatermarkBmp = nullptr;
static std::mutex g_bmpMutex;

// --- UTF8 helpers & json getters ---
static std::wstring Utf8ToUtf16(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &ws[0], len);
    return ws;
}
static std::wstring jget_w(const json& j, const char* key, const std::wstring& def = L"") {
    if (!j.contains(key) || !j[key].is_string()) return def;
    return Utf8ToUtf16(j[key].get<std::string>());
}
static float jget_f(const json& j, const char* key, float def = 0.0f) {
    if (!j.contains(key)) return def;
    if (j[key].is_number_float()) return (float)j[key].get<double>();
    if (j[key].is_number_integer()) return (float)j[key].get<long long>();
    return def;
}
static int jget_i(const json& j, const char* key, int def = 0) {
    if (!j.contains(key)) return def;
    if (j[key].is_number_integer()) return (int)j[key].get<long long>();
    if (j[key].is_number_float())  return (int)j[key].get<double>();
    return def;
}
static bool jget_b(const json& j, const char* key, bool def = false) {
    if (!j.contains(key) || !j[key].is_boolean()) return def;
    return j[key].get<bool>();
}

static bool TryParseSnapshotFromJson(const json& j, RenderSnapshot& outSnap) {
    if (!j.contains("MarkerJson") || !j["MarkerJson"].is_string()) return false;
    try {
        std::string mj_str = j["MarkerJson"].get<std::string>();
        auto m = json::parse(mj_str);
        outSnap.DrawingEnabled = jget_b(m, "DrawingEnabled", true);
        outSnap.Opacity = jget_f(m, "Opacity", 1.0f);
        {
            std::wstring custom = jget_w(m, "TextCustomDateTimeFormat", L"");
            std::wstring tsfmt = jget_w(m, "TimestampFormat", L"");
            if (!custom.empty()) outSnap.TimestampFormat = custom;
            else if (!tsfmt.empty()) outSnap.TimestampFormat = tsfmt;
            else outSnap.TimestampFormat = L"HH:mm:ss dd/MM/yyyy";
        }
        outSnap.TextEnabled = jget_b(m, "TextEnabled", false);
        outSnap.TextFormat = jget_w(m, "TextFormat", L"{machinename} | {shortdate} {shorttime}");
        outSnap.TextSize = (float)jget_i(m, "TextSize", 28);
        outSnap.TextOpacity = jget_f(m, "TextOpacity", 0.25f);
        outSnap.TextBlurRadius = jget_f(m, "TextBlurRadius", 0.0f);
        outSnap.TextAdjustment = jget_b(m, "TextAdjustment", false);
        outSnap.TextSpacingEnabled = jget_b(m, "TextSpacingEnabled", true);
        outSnap.TextSpacingX = (float)jget_i(m, "TextSpacingX", 320);
        outSnap.TextSpacingY = (float)jget_i(m, "TextSpacingY", 160);
        outSnap.TextCols = jget_i(m, "TextCols", 4);
        outSnap.TextRows = jget_i(m, "TextRows", 3);
        outSnap.TextColor1 = jget_w(m, "TextColor1", L"#000000");
        outSnap.TextColor2 = jget_w(m, "TextColor2", L"#FFFFFF");
        {
            int ang = jget_i(m, "TextAngle", 0);
            outSnap.TextAngleDeg = RenderSnapshot::NormalizeAngleDeg((float)ang);
        }
        return true;
    } catch (...) { return false; }
}

static void ProcessPipeLineBuffer(std::string& buffer) {
    while (true) {
        size_t pos = buffer.find('\n');
        if (pos == std::string::npos) break;
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            RenderSnapshot snap;
            if (TryParseSnapshotFromJson(j, snap)) {
                RendererManager::Instance().SetSnapshot(snap);
            }
        } catch (...) {}
    }
}

static DWORD WINAPI IpcClientThread(LPVOID) {
    DebugLog::log("[WebcamDLL][IPC] Client thread started");
    std::string buffer;
    while (!g_bUnloading.load()) {
        HANDLE hPipe = CreateFileW(kPipeInject, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
        char tmp[2048]; DWORD cb = 0;
        while (!g_bUnloading.load() && ReadFile(hPipe, tmp, sizeof(tmp), &cb, nullptr) && cb > 0) {
            buffer.append(tmp, cb);
            ProcessPipeLineBuffer(buffer);
        }
        CloseHandle(hPipe);
    }
    DebugLog::log("[WebcamDLL][IPC] Client thread exited");
    return 0;
}

// --- Macro Expansion Helper ---
static std::wstring ExpandMacros(std::wstring text) {
    auto ReplaceAll = [&](const std::wstring& search, const std::wstring& replace) {
        size_t pos = 0;
        std::wstring searchLower = search; for (auto& c : searchLower) c = towlower(c);
        while (true) {
            std::wstring textLower = text; for (auto& c : textLower) c = towlower(c);
            pos = textLower.find(searchLower, pos);
            if (pos == std::wstring::npos) break;
            text.replace(pos, search.length(), replace);
            pos += replace.length();
        }
    };
    wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1]; DWORD sz = ARRAYSIZE(comp);
    if (GetComputerNameW(comp, &sz)) ReplaceAll(L"{MachineName}", comp);
    wchar_t user[256]; DWORD usz = ARRAYSIZE(user);
    if (GetUserNameW(user, &usz)) ReplaceAll(L"{UserName}", user);
    SYSTEMTIME st; GetLocalTime(&st); wchar_t buf[64];
    swprintf_s(buf, L"%02d/%02d/%04d", st.wDay, st.wMonth, st.wYear); ReplaceAll(L"{ShortDate}", buf);
    swprintf_s(buf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond); ReplaceAll(L"{ShortTime}", buf);
    return text;
}

// --- GDI+ Drawing Core ---
void UpdateWatermarkBitmap(const RenderSnapshot& snap, int width, int height) {
    std::lock_guard<std::mutex> lock(g_bmpMutex);
    if (g_pWatermarkBmp) { delete g_pWatermarkBmp; g_pWatermarkBmp = nullptr; }
    if (!snap.DrawingEnabled || !snap.TextEnabled || g_bUnloading.load()) return;

    g_pWatermarkBmp = new Gdiplus::Bitmap(width, height, PixelFormat32bppARGB);
    Gdiplus::Graphics g(g_pWatermarkBmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    std::wstring text = ExpandMacros(snap.TextFormat);
    Gdiplus::FontFamily ff(L"Arial");
    Gdiplus::Font font(&ff, snap.TextSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    
    int r = 255, gc = 255, b = 255;
    if (snap.TextColor1.size() == 7 && snap.TextColor1[0] == '#') swscanf_s(snap.TextColor1.c_str(), L"#%02x%02x%02x", &r, &gc, &b);
    Gdiplus::SolidBrush brush(Gdiplus::Color((BYTE)(snap.TextOpacity * 255), (BYTE)r, (BYTE)gc, (BYTE)b));

    int sx = (int)snap.TextSpacingX; int sy = (int)snap.TextSpacingY;
    if (sx < 50) sx = 300; if (sy < 50) sy = 200;

    for (int y = 0; y < height; y += sy) {
        for (int x = 0; x < width; x += sx) {
            g.ResetTransform(); 
            if (g_isMirrorMode) {
                g.ScaleTransform(-1.0f, 1.0f);
                g.TranslateTransform(-(float)width, 0.0f);
            }
            g.TranslateTransform((float)x, (float)y); g.RotateTransform(snap.TextAngleDeg);
            g.DrawString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), &brush);
        }
    }
}

void BlendARGBtoYUY2(BYTE* pData, int width, int height, int stride, Gdiplus::Bitmap* pBmp) {
    if (!pBmp || g_bUnloading.load()) return;
    Gdiplus::BitmapData bd; Gdiplus::Rect rc(0, 0, width, height);
    if (pBmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
        BYTE* pSrc = (BYTE*)bd.Scan0;
        if (stride == 0) stride = width * 2;
        for (int y = 0; y < height; y++) {
            if (g_bUnloading.load()) break;
            for (int x = 0; x < width; x++) {
                BYTE* pPx = pSrc + (y * bd.Stride) + (x * 4); BYTE a = pPx[3];
                if (a > 0) {
                    int r = pPx[2], g = pPx[1], b = pPx[0];
                    BYTE Y = (BYTE)((0.299 * r) + (0.587 * g) + (0.114 * b));
                    BYTE U = (BYTE)(-(0.1687 * r) - (0.3313 * g) + (0.5 * b) + 128);
                    BYTE V = (BYTE)((0.5 * r) - (0.4187 * g) - (0.0813 * b) + 128);
                    int pos = y * stride + x * 2;
                    if (a == 255) { pData[pos] = Y; if (x % 2 == 0) { pData[pos+1] = U; pData[pos+3] = V; } }
                    else { float f = a / 255.0f; pData[pos] = (BYTE)(pData[pos] * (1 - f) + Y * f); }
                }
            }
        }
        pBmp->UnlockBits(&bd);
    }
}

void BlendARGBtoNV12(BYTE* pY, BYTE* pUV, int width, int height, int stride, Gdiplus::Bitmap* pBmp) {
    if (!pBmp || g_bUnloading.load()) return;
    Gdiplus::BitmapData bd; Gdiplus::Rect rc(0, 0, width, height);
    if (pBmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
        BYTE* pSrc = (BYTE*)bd.Scan0;
        if (stride == 0) stride = width;
        for (int y = 0; y < height; y++) {
            if (g_bUnloading.load()) break;
            for (int x = 0; x < width; x++) {
                BYTE* pPx = pSrc + (y * bd.Stride) + (x * 4); BYTE a = pPx[3];
                if (a > 0) {
                    int r = pPx[2], g = pPx[1], b = pPx[0];
                    BYTE Y = (BYTE)((0.299 * r) + (0.587 * g) + (0.114 * b));
                    BYTE U = (BYTE)(-(0.1687 * r) - (0.3313 * g) + (0.5 * b) + 128);
                    BYTE V = (BYTE)((0.5 * r) - (0.4187 * g) - (0.0813 * b) + 128);
                    if (a == 255) { pY[y * stride + x] = Y; if (x % 2 == 0 && y % 2 == 0) { int up = (y / 2) * stride + x; pUV[up] = U; pUV[up+1] = V; } }
                    else { float f = a / 255.0f; pY[y * stride + x] = (BYTE)(pY[y * stride + x] * (1 - f) + Y * f); }
                }
            }
        }
        pBmp->UnlockBits(&bd);
    }
}

// Actual drawing logic with C++ objects
static void ProcessWatermarkInternal(BYTE* pData, int width, int height, bool isNV12, int stride) {
    if (g_bUnloading.load()) return;
    std::lock_guard<std::mutex> lock(g_drawMutex);
    if (stride == 0) stride = isNV12 ? width : width * 2;
    if (RendererManager::Instance().HasSnapshot()) {
        const auto& snap = RendererManager::Instance().GetSnapshot();
        static uint32_t lastSig = 0; uint32_t sig = (uint32_t)snap.Signature();
        if (sig != lastSig || !g_pWatermarkBmp) { UpdateWatermarkBitmap(snap, width, height); lastSig = sig; }
        if (g_pWatermarkBmp) {
            std::lock_guard<std::mutex> bmpLock(g_bmpMutex);
            if (isNV12) BlendARGBtoNV12(pData, pData + stride * height, width, height, stride, g_pWatermarkBmp);
            else BlendARGBtoYUY2(pData, width, height, stride, g_pWatermarkBmp);
            return;
        }
    } else {
        static std::atomic<int> logCounter(0);
        if (logCounter.fetch_add(1) % 300 == 0) DebugLog::log("[WebcamDLL] No snapshot available, skipping watermark.");
    }
}

// Strictly follow SEH rules
#pragma runtime_checks("", off)
static void SafeDrawWrapper(BYTE* pData, int width, int height, bool isNV12, int stride) {
    __try {
        ProcessWatermarkInternal(pData, width, height, isNV12, stride);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
#pragma runtime_checks("", restore)

void ProcessWatermark(BYTE* pData, int width, int height, bool isNV12, int stride = 0) {
    if (g_bUnloading.load() || !pData) return;
    g_activeCalls++;
    SafeDrawWrapper(pData, width, height, isNV12, stride);
    g_activeCalls--;
}

// --- Hooks ---
typedef HRESULT(WINAPI* PMFCreateSourceReaderFromMediaSource)(IMFMediaSource*, IMFAttributes*, IMFSourceReader**);
static PMFCreateSourceReaderFromMediaSource g_origMFCreateSourceReaderMS = NULL;
typedef HRESULT(WINAPI* PMFCreateSourceReaderFromUnknown)(IUnknown*, IMFAttributes*, IMFSourceReader**);
static PMFCreateSourceReaderFromUnknown g_origMFCreateSourceReaderUnk = NULL;
typedef HRESULT(WINAPI* PMFCreateSourceReaderFromByteStream)(IMFByteStream*, IMFAttributes*, IMFSourceReader**);
static PMFCreateSourceReaderFromByteStream g_origMFCreateSourceReaderBS = NULL;
typedef HRESULT(STDMETHODCALLTYPE* POnReadSample)(IMFSourceReaderCallback*, HRESULT, DWORD, DWORD, LONGLONG, IMFSample*);
static POnReadSample g_origOnReadSample = NULL;
typedef HRESULT(STDMETHODCALLTYPE* PReadSample)(IMFSourceReader*, DWORD, DWORD, DWORD*, DWORD*, LONGLONG*, IMFSample**);
static PReadSample g_origReadSample = NULL;

void ProcessMFSample(IMFSourceReader* pReader, IMFSample* pS) {
    if (!pS || g_bUnloading.load()) return;
    IMFMediaBuffer* pB = NULL;
    if (FAILED(pS->ConvertToContiguousBuffer(&pB))) return;

    BYTE* pD = NULL; DWORD maxLen = 0, curLen = 0;
    if (SUCCEEDED(pB->Lock(&pD, &maxLen, &curLen))) {
        int w = 640, h = 480, stride = 0;
        bool isNV12 = false;

        if (pReader) {
            IMFMediaType* pType = NULL;
            if (SUCCEEDED(pReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pType))) {
                UINT32 width = 0, height = 0;
                MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &width, &height);
                if (width > 0 && height > 0) { w = (int)width; h = (int)height; }
                GUID subtype;
                if (SUCCEEDED(pType->GetGUID(MF_MT_SUBTYPE, &subtype))) isNV12 = (subtype == MFVideoFormat_NV12);
                UINT32 s = 0;
                if (SUCCEEDED(pType->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) stride = (int)s;
                pType->Release();
            }
        }

        IMF2DBuffer* p2B = NULL;
        if (SUCCEEDED(pB->QueryInterface(IID_IMF2DBuffer, (void**)&p2B))) {
            BYTE* pScan0 = NULL; LONG lStride = 0;
            if (SUCCEEDED(p2B->Lock2D(&pScan0, &lStride))) {
                ProcessWatermark(pScan0, w, h, isNV12, (int)abs(lStride));
                p2B->Unlock2D();
            }
            p2B->Release();
        } else {
            if (stride == 0) stride = isNV12 ? w : w * 2;
            ProcessWatermark(pD, w, h, isNV12, stride);
        }
        pB->Unlock();
    }
    pB->Release();
}

HRESULT STDMETHODCALLTYPE HookedOnReadSample(IMFSourceReaderCallback* pS, HRESULT hr, DWORD di, DWORD df, LONGLONG ts, IMFSample* sa) {
    if (SUCCEEDED(hr) && sa) ProcessMFSample(nullptr, sa); return g_origOnReadSample(pS, hr, di, df, ts, sa);
}
HRESULT STDMETHODCALLTYPE HookedReadSample(IMFSourceReader* pS, DWORD di, DWORD df, DWORD* ad, DWORD* sf, LONGLONG* ts, IMFSample** sa) {
    HRESULT hr = g_origReadSample(pS, di, df, ad, sf, ts, sa); if (SUCCEEDED(hr) && sa && *sa) ProcessMFSample(pS, *sa); return hr;
}

void HookSourceReader(IMFSourceReader* pR, IMFAttributes* pA) {
    if (!pR) return; std::lock_guard<std::mutex> lk(g_hookMutex);
    if (!g_bMFHooked.load()) {
        void** vt = *(void***)pR; if (MH_CreateHook(vt[9], &HookedReadSample, (LPVOID*)&g_origReadSample) == MH_OK) { MH_EnableHook(vt[9]); g_bMFHooked = true; }
    }
    if (pA && !g_bMFCallbackHooked.load()) {
        IUnknown* pC = NULL; if (SUCCEEDED(pA->GetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, IID_IUnknown, (LPVOID*)&pC))) {
            void** vt = *(void***)pC; if (MH_CreateHook(vt[3], &HookedOnReadSample, (LPVOID*)&g_origOnReadSample) == MH_OK) { MH_EnableHook(vt[3]); g_bMFCallbackHooked = true; }
            pC->Release();
        }
    }
}

HRESULT WINAPI HookedMFCreateSourceReaderFromMediaSource(IMFMediaSource* pM, IMFAttributes* pA, IMFSourceReader** pS) {
    HRESULT hr = g_origMFCreateSourceReaderMS(pM, pA, pS); if (SUCCEEDED(hr) && pS && *pS) HookSourceReader(*pS, pA); return hr;
}
HRESULT WINAPI HookedMFCreateSourceReaderFromUnknown(IUnknown* pU, IMFAttributes* pA, IMFSourceReader** pS) {
    HRESULT hr = g_origMFCreateSourceReaderUnk(pU, pA, pS); if (SUCCEEDED(hr) && pS && *pS) HookSourceReader(*pS, pA); return hr;
}
HRESULT WINAPI HookedMFCreateSourceReaderFromByteStream(IMFByteStream* pB, IMFAttributes* pA, IMFSourceReader** pS) {
    HRESULT hr = g_origMFCreateSourceReaderBS(pB, pA, pS); if (SUCCEEDED(hr) && pS && *pS) HookSourceReader(*pS, pA); return hr;
}

// --- DirectShow Hooks ---
typedef HRESULT(WINAPI* PCoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static PCoCreateInstance g_origCoCreateInstance = NULL;
typedef HRESULT(STDMETHODCALLTYPE* PGraphConnect)(IGraphBuilder*, IPin*, IPin*);
static PGraphConnect g_origGraphConnect = NULL;
typedef HRESULT(STDMETHODCALLTYPE* PReceive)(IMemInputPin*, IMediaSample*);
static PReceive g_origReceive = NULL;

HRESULT STDMETHODCALLTYPE HookedReceive(IMemInputPin* pS, IMediaSample* pM) {
    if (pM && !g_bUnloading.load()) {
        BYTE* pB = NULL; if (SUCCEEDED(pM->GetPointer(&pB))) {
            int w = 640, h = 480, stride = 0; bool isNV12 = false;
            {
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                if (g_videoConfigs.count(pS)) {
                    const auto& cfg = g_videoConfigs[pS];
                    w = cfg.width; h = cfg.height; stride = cfg.stride; isNV12 = cfg.isNV12;
                } else {
                    long len = pM->GetActualDataLength();
                    if (len >= 1280 * 720 * 2) { w = 1280; h = 720; }
                }
            }
            ProcessWatermark(pB, w, h, isNV12, stride);
        }
    }
    return g_origReceive(pS, pM);
}

HRESULT STDMETHODCALLTYPE HookedGraphConnect(IGraphBuilder* pS, IPin* pO, IPin* pI) {
    HRESULT hr = g_origGraphConnect(pS, pO, pI);
    if (SUCCEEDED(hr)) {
        AM_MEDIA_TYPE mt;
        if (SUCCEEDED(pI->ConnectionMediaType(&mt))) {
            if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
                VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)mt.pbFormat;
                VideoConfig cfg;
                cfg.width = vih->bmiHeader.biWidth;
                cfg.height = (int)abs(vih->bmiHeader.biHeight);
                cfg.isNV12 = (mt.subtype == MEDIASUBTYPE_NV12);
                cfg.stride = (int)(cfg.isNV12 ? cfg.width : (cfg.width * 2));
                std::lock_guard<std::mutex> lk(g_cfgMutex);
                IMemInputPin* pM = NULL; 
                if (SUCCEEDED(pI->QueryInterface(IID_IMemInputPin, (void**)&pM))) {
                    g_videoConfigs[pM] = cfg;
                    if (!g_bDShowHooked.load()) {
                        void** vt = *(void***)pM; 
                        if (MH_CreateHook(vt[6], &HookedReceive, (LPVOID*)&g_origReceive) == MH_OK) {
                            MH_EnableHook(vt[6]); g_bDShowHooked = true;
                        }
                    }
                    pM->Release();
                }
            }
            FreeMediaType(mt);
        }
    }
    return hr;
}

HRESULT WINAPI HookedCoCreateInstance(REFCLSID clsid, LPUNKNOWN pU, DWORD ctx, REFIID riid, LPVOID* ppv) {
    HRESULT hr = g_origCoCreateInstance(clsid, pU, ctx, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (riid == IID_IGraphBuilder || riid == IID_IFilterGraph) {
            std::lock_guard<std::mutex> lk(g_hookMutex);
            static void* lastVT = nullptr; void** vt = *(void***)*ppv;
            if (vt != lastVT) { if (MH_CreateHook(vt[11], &HookedGraphConnect, (LPVOID*)&g_origGraphConnect) == MH_OK) { MH_EnableHook(vt[11]); lastVT = vt; } }
        }
    }
    return hr;
}

// --- Watchdog ---
DWORD WINAPI WatchdogThread(LPVOID) {
    while (!g_bUnloading.load()) {
        Sleep(3000); 
        HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{sizeof(pe)}; bool found = false;
        if (Process32FirstW(h, &pe)) { do { if (_wcsicmp(pe.szExeFile, L"AgileMark.exe") == 0) { found = true; break; } } while (Process32NextW(h, &pe)); }
        CloseHandle(h);
        if (!found) {
            DebugLog::log("[WebcamDLL] AgileMark not found. Entering Zombie Mode...");
            g_bUnloading = true;
            while (g_activeCalls.load() > 0) Sleep(50);
            MH_DisableHook(MH_ALL_HOOKS);
            Sleep(3000);
            DebugLog::log("[WebcamDLL] Safe to terminate. Goodbye.");
            FreeLibraryAndExitThread(g_hModule, 0);
        }
    }
    return 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI StartWatch(LPVOID lp) {
    if (g_bInitialized.exchange(true)) return 0;
    DebugLog::initialize(); DebugLog::log("[WebcamDLL] StartWatch v18.4.0 (Dynamic Resolution)");

    wchar_t modPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, modPath, MAX_PATH)) {
        std::wstring path(modPath);
        for (auto& c : path) c = towlower(c);
        if (path.find(L"zoom.exe") != std::wstring::npos || path.find(L"teams.exe") != std::wstring::npos) {
            g_isMirrorMode = true;
            DebugLog::log("[WebcamDLL] Mirror Mode enabled for this process.");
        }
    }

    Gdiplus::GdiplusStartupInput gsi; Gdiplus::GdiplusStartup(&g_gdiplusToken, &gsi, NULL);
    g_hIpcThread = CreateThread(NULL, 0, IpcClientThread, NULL, 0, NULL);
    if (MH_Initialize() == MH_OK) {
        HMODULE hO = GetModuleHandleW(L"ole32.dll"); if (hO) MH_CreateHook((void*)GetProcAddress(hO, "CoCreateInstance"), &HookedCoCreateInstance, (LPVOID*)&g_origCoCreateInstance);
        HMODULE hM = GetModuleHandleW(L"Mfreadwrite.dll"); if (!hM) hM = LoadLibraryW(L"Mfreadwrite.dll");
        if (hM) {
            void* p1 = (void*)GetProcAddress(hM, "MFCreateSourceReaderFromMediaSource");
            void* p2 = (void*)GetProcAddress(hM, "MFCreateSourceReaderFromUnknown");
            void* p3 = (void*)GetProcAddress(hM, "MFCreateSourceReaderFromByteStream");
            if (p1) MH_CreateHook(p1, &HookedMFCreateSourceReaderFromMediaSource, (LPVOID*)&g_origMFCreateSourceReaderMS);
            if (p2) MH_CreateHook(p2, &HookedMFCreateSourceReaderFromUnknown, (LPVOID*)&g_origMFCreateSourceReaderUnk);
            if (p3) MH_CreateHook(p3, &HookedMFCreateSourceReaderFromByteStream, (LPVOID*)&g_origMFCreateSourceReaderBS);
        }
        MH_EnableHook(MH_ALL_HOOKS);
    }
    CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hMod); g_hModule = hMod; }
    return TRUE;
}
