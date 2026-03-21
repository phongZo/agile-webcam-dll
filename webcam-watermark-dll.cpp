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
#include <algorithm>
#include <vector>
#include "DebugLog.h"
#include "../packages/minhook.1.3.3/lib/native/include/MinHook.h"
#include <nlohmann/json.hpp>
#include "Renderer/RendererManager.h"
#include "Renderer/RenderSnapshot.h"
#include "JpegHelper.h"

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")

using json = nlohmann::json;

#ifdef _WIN64
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x64-v141-mt.lib")
#else
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x86-v141-mt.lib")
#endif

// --- Core Helpers ---
static void FreeMediaType(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat != 0) { CoTaskMemFree((PVOID)mt.pbFormat); mt.pbFormat = NULL; }
    if (mt.pUnk != NULL) { mt.pUnk->Release(); mt.pUnk = NULL; }
}

#ifndef MEDIASUBTYPE_NV12
DEFINE_GUID(MEDIASUBTYPE_NV12, 0x3231564e, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
#endif
#ifndef MEDIASUBTYPE_MJPG
DEFINE_GUID(MEDIASUBTYPE_MJPG, 0x47504A4D, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
#endif

// --- Global State ---
static HINSTANCE g_hModule = NULL;
static std::atomic<bool> g_bUnloading(false);
static std::atomic<bool> g_bInitialized(false);
static std::atomic<int> g_activeCalls(0); 
static std::mutex g_hookMutex;
static std::mutex g_drawMutex;
static ULONG_PTR g_gdiplusToken = 0;
static bool g_isMirrorMode = false;

static std::atomic<bool> g_bDShowHooked(false);
static std::atomic<bool> g_bMFHooked(false);

struct BufferTag { DWORD timestamp; };
static std::map<void*, BufferTag> g_processedRawBuffers;
static std::mutex g_rawBufferMutex;

struct VideoConfig {
    int width = 0, height = 0;
    bool isNV12 = false, isCompressed = false;
};
static std::map<void*, VideoConfig> g_videoConfigs;
static std::mutex g_cfgMutex;

struct ResKey {
    int w, h; uint32_t sig; std::wstring text;
    bool operator<(const ResKey& o) const { 
        if(w != o.w) return w < o.w; if(h != o.h) return h < o.h; 
        if(sig != o.sig) return sig < o.sig; return text < o.text; 
    }
};
static std::map<ResKey, Gdiplus::Bitmap*> g_resBmpCache;
static std::mutex g_cacheMutex;

static std::map<void*, void*> g_mfCallbackToReader;
static std::mutex g_mfMapMutex;

static const wchar_t* kPipeInject = L"\\\\.\\pipe\\AgileMarkPipe_qaKOab5VPyK4ar4A6sfm2VZ0";

// --- Advanced String Helpers ---
static std::wstring Utf8ToUtf16(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &ws[0], len);
    return ws;
}

static void ReplaceAllCI(std::wstring& text, const std::wstring& search, const std::wstring& replace) {
    if (search.empty() || text.empty()) return;
    std::wstring sl = search; std::transform(sl.begin(), sl.end(), sl.begin(), ::towlower);
    size_t pos = 0;
    while (true) {
        std::wstring tl = text; std::transform(tl.begin(), tl.end(), tl.begin(), ::towlower);
        pos = tl.find(sl, pos);
        if (pos == std::wstring::npos) break;
        text.replace(pos, search.length(), replace);
        pos += replace.length();
    }
}

static std::wstring ExpandMacros(std::wstring text) {
    wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1] = {0}; DWORD sz = ARRAYSIZE(comp);
    if (GetComputerNameW(comp, &sz)) { ReplaceAllCI(text, L"{MachineName}", comp); ReplaceAllCI(text, L"{machinename}", comp); }
    wchar_t user[256] = {0}; DWORD usz = ARRAYSIZE(user);
    if (GetUserNameW(user, &usz)) { ReplaceAllCI(text, L"{UserName}", user); ReplaceAllCI(text, L"{username}", user); }
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t sd[32], stm[32]; swprintf_s(sd, L"%02d/%02d/%04d", st.wDay, st.wMonth, st.wYear); swprintf_s(stm, L"%02d:%02d", st.wHour, st.wMinute);
    ReplaceAllCI(text, L"{ShortDate}", sd); ReplaceAllCI(text, L"{shortdate}", sd);
    ReplaceAllCI(text, L"{ShortTime}", stm); ReplaceAllCI(text, L"{shorttime}", stm);
    return text;
}

static bool TryParseSnapshotFromJson(const json& j, RenderSnapshot& outSnap) {
    if (!j.contains("MarkerJson") || !j["MarkerJson"].is_string()) return false;
    try {
        auto m = json::parse(j["MarkerJson"].get<std::string>());
        outSnap.TextEnabled = m.value("TextEnabled", true);
        outSnap.TextFormat = Utf8ToUtf16(m.value("TextFormat", "{machinename} | {username}"));
        outSnap.TextSize = (float)m.value("TextSize", 28);
        outSnap.TextOpacity = m.value("TextOpacity", 0.5f);
        outSnap.TextAngleDeg = (float)m.value("TextAngle", -20.0f);
        outSnap.TextSpacingEnabled = m.value("TextSpacingEnabled", true);
        outSnap.TextSpacingX = (float)m.value("TextSpacingX", 300.0f);
        outSnap.TextSpacingY = (float)m.value("TextSpacingY", 150.0f);
        outSnap.TextCols = m.value("TextCols", 4);
        outSnap.TextRows = m.value("TextRows", 3);
        outSnap.TextColor1 = Utf8ToUtf16(m.value("TextColor1", "#000000"));
        outSnap.TextColor2 = Utf8ToUtf16(m.value("TextColor2", "#FFFFFF"));
        outSnap.Opacity = m.value("Opacity", 1.0f);
        outSnap.DrawingEnabled = m.value("DrawingEnabled", true);
        return true;
    } catch (...) { return false; }
}

// --- Anti-Double Exposure ---
bool IsRawFrameAlreadyProcessed(void* pData) {
    if (!pData) return false;
    DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lock(g_rawBufferMutex);
    auto it = g_processedRawBuffers.find(pData);
    if (it != g_processedRawBuffers.end() && (now - it->second.timestamp < 15)) return true;
    g_processedRawBuffers[pData] = { now };
    return false;
}

// --- Blending ---
void BlendARGBtoYUY2(BYTE* pData, int width, int height, int stride, Gdiplus::Bitmap* pBmp) {
    Gdiplus::BitmapData bd; Gdiplus::Rect rc(0, 0, pBmp->GetWidth(), pBmp->GetHeight());
    if (pBmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
        BYTE* pSrc = (BYTE*)bd.Scan0;
        int dW = (std::min)(width, (int)pBmp->GetWidth()), dH = (std::min)(height, (int)pBmp->GetHeight());
        for (int y = 0; y < dH; y++) {
            for (int x = 0; x < dW; x++) {
                BYTE* pS = pSrc + (y * bd.Stride) + (x * 4);
                int alpha = pS[3];
                if (alpha > 30) {
                    int invA = 255 - alpha;
                    int base = y * stride + (x / 2) * 4;
                    int yP = base + (x % 2) * 2;
                    BYTE Y = (BYTE)((0.299 * pS[2]) + (0.587 * pS[1]) + (0.114 * pS[0]));
                    BYTE U = (BYTE)(-(0.1687 * pS[2]) - (0.3313 * pS[1]) + (0.5 * pS[0]) + 128);
                    BYTE V = (BYTE)((0.5 * pS[2]) - (0.4187 * pS[1]) - (0.0813 * pS[0]) + 128);
                    pData[yP] = (BYTE)((Y * alpha + pData[yP] * invA) >> 8);
                    pData[base+1] = (BYTE)((U * alpha + pData[base+1] * invA) >> 8);
                    pData[base+3] = (BYTE)((V * alpha + pData[base+3] * invA) >> 8);
                }
            }
        }
        pBmp->UnlockBits(&bd);
    }
}

void BlendARGBtoNV12(BYTE* pY, BYTE* pUV, int width, int height, int stride, Gdiplus::Bitmap* pBmp) {
    Gdiplus::BitmapData bd; Gdiplus::Rect rc(0, 0, pBmp->GetWidth(), pBmp->GetHeight());
    if (pBmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
        BYTE* pSrc = (BYTE*)bd.Scan0;
        int dW = (std::min)(width, (int)pBmp->GetWidth()), dH = (std::min)(height, (int)pBmp->GetHeight());
        for (int y = 0; y < dH; y++) {
            for (int x = 0; x < dW; x++) {
                BYTE* pS = pSrc + (y * bd.Stride) + (x * 4);
                int alpha = pS[3];
                if (alpha > 30) {
                    int invA = 255 - alpha; int yPos = y * stride + x;
                    int uvIdx = (y / 2) * stride + (x / 2) * 2;
                    BYTE Y = (BYTE)((0.299 * pS[2]) + (0.587 * pS[1]) + (0.114 * pS[0]));
                    BYTE U = (BYTE)(-(0.1687 * pS[2]) - (0.3313 * pS[1]) + (0.5 * pS[0]) + 128);
                    BYTE V = (BYTE)((0.5 * pS[2]) - (0.4187 * pS[1]) - (0.0813 * pS[0]) + 128);
                    pY[yPos] = (BYTE)((Y * alpha + pY[yPos] * invA) >> 8);
                    pUV[uvIdx] = (BYTE)((U * alpha + pUV[uvIdx] * invA) >> 8);
                    pUV[uvIdx+1] = (BYTE)((V * alpha + pUV[uvIdx+1] * invA) >> 8);
                }
            }
        }
        pBmp->UnlockBits(&bd);
    }
}

void BlendARGBtoBGRA(BYTE* pData, int width, int height, int stride, Gdiplus::Bitmap* pBmp) {
    Gdiplus::BitmapData bd; Gdiplus::Rect rc(0, 0, pBmp->GetWidth(), pBmp->GetHeight());
    if (pBmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
        BYTE* pSrc = (BYTE*)bd.Scan0;
        int dW = (std::min)(width, (int)pBmp->GetWidth()), dH = (std::min)(height, (int)pBmp->GetHeight());
        int bpp = stride / width;
        for (int y = 0; y < dH; y++) {
            for (int x = 0; x < dW; x++) {
                BYTE* pS = pSrc + (y * bd.Stride) + (x * 4);
                int alpha = pS[3];
                if (alpha > 30) {
                    BYTE* pD = pData + (y * stride) + (x * bpp);
                    int invA = 255 - alpha;
                    pD[0] = (BYTE)((pS[0] * alpha + pD[0] * invA) >> 8);
                    pD[1] = (BYTE)((pS[1] * alpha + pD[1] * invA) >> 8);
                    pD[2] = (BYTE)((pS[2] * alpha + pD[2] * invA) >> 8);
                    if (bpp == 4) pD[3] = 255;
                }
            }
        }
        pBmp->UnlockBits(&bd);
    }
}

Gdiplus::Bitmap* GetWatermarkForRes(const RenderSnapshot& snap, int width, int height) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    std::wstring expanded = ExpandMacros(snap.TextFormat);
    ResKey key = { width, height, (uint32_t)snap.Signature(), expanded };
    if (g_resBmpCache.count(key)) return g_resBmpCache[key];

    for (auto it = g_resBmpCache.begin(); it != g_resBmpCache.end(); ) {
        if (it->first.w == width && it->first.h == height) { delete it->second; it = g_resBmpCache.erase(it); } else ++it;
    }

    Gdiplus::Bitmap* pBmp = new Gdiplus::Bitmap(width, height, PixelFormat32bppARGB);
    Gdiplus::Graphics g(pBmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    Gdiplus::FontFamily ff(L"Arial");
    Gdiplus::Font font(&ff, snap.TextSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    unsigned int r1=0,g1=0,b1=0,r2=255,g2=255,b2=255;
    swscanf_s(snap.TextColor1.c_str(), L"#%02x%02x%02x", &r1, &g1, &b1);
    swscanf_s(snap.TextColor2.c_str(), L"#%02x%02x%02x", &r2, &g2, &b2);
    BYTE alpha = (BYTE)(snap.TextOpacity * 255);
    Gdiplus::SolidBrush b1s(Gdiplus::Color(alpha, (BYTE)r1, (BYTE)g1, (BYTE)b1));
    Gdiplus::SolidBrush b2s(Gdiplus::Color(alpha, (BYTE)r2, (BYTE)g2, (BYTE)b2));

    double bW, bH, pT = 0, pL = 0;
    if (!snap.TextSpacingEnabled) {
        bW = (double)width / (snap.TextCols > 0 ? snap.TextCols : 1);
        bH = (double)height / (snap.TextRows > 0 ? snap.TextRows : 1);
        Gdiplus::RectF br; g.MeasureString(expanded.c_str(), -1, &font, Gdiplus::PointF(0, 0), &br);
        pT = (bH - br.Height) / 2.0; pL = (bW - br.Width) / 2.0;
    } else {
        bW = (std::max)(150.0f, snap.TextSpacingX); bH = (std::max)(100.0f, snap.TextSpacingY);
    }

    bool wb = false;
    for (double y = pT; y < height; y += bH) {
        for (double x = pL; x < width; x += bW) {
            wb = !wb; g.ResetTransform();
            if (g_isMirrorMode) { g.ScaleTransform(-1.0f, 1.0f); g.TranslateTransform(-(float)width, 0.0f); }
            g.TranslateTransform((float)x, (float)y); g.RotateTransform(snap.TextAngleDeg);
            g.DrawString(expanded.c_str(), -1, &font, Gdiplus::PointF(0, 0), wb ? &b1s : &b2s);
        }
    }
    g_resBmpCache[key] = pBmp;
    return pBmp;
}

static void ProcessWatermarkInternal(BYTE* pData, int width, int height, int formatType, int stride, bool isCompressed) {
    if (g_bUnloading.load() || isCompressed || !pData || width <= 0 || height <= 0) return;
    std::lock_guard<std::mutex> lock(g_drawMutex);
    if (g_gdiplusToken == 0) return;
    if (RendererManager::Instance().HasSnapshot()) {
        const auto& snap = RendererManager::Instance().GetSnapshot();
        if (!snap.TextEnabled) return;
        Gdiplus::Bitmap* pBmp = GetWatermarkForRes(snap, width, height);
        if (pBmp) {
            if (formatType == 1) BlendARGBtoNV12(pData, pData + (stride * height), width, height, stride, pBmp);
            else if (formatType == 0) BlendARGBtoYUY2(pData, width, height, stride, pBmp);
            else if (formatType == 2) BlendARGBtoBGRA(pData, width, height, stride, pBmp);
        }
    }
}

// --- MJPG Engine ---
void ProcessMJPGFrame(BYTE* pData, DWORD curL, DWORD maxL, IMFMediaBuffer* pB, IMediaSample* pM) {
    std::vector<BYTE> rgba; int dw, dh;
    if (JpegHelper::DecompressMJPG(pData, curL, dw, dh, rgba)) {
        int bpp = (int)rgba.size() / (dw * dh);
        ProcessWatermarkInternal(rgba.data(), dw, dh, 2, dw * bpp, false);
        std::vector<BYTE> nj; int q = 85;
        while (q >= 40) {
            if (JpegHelper::CompressMJPG(rgba.data(), dw, dh, nj, q)) {
                if (nj.size() <= maxL) {
                    memcpy(pData, nj.data(), nj.size());
                    if (pB) pB->SetCurrentLength((DWORD)nj.size());
                    if (pM) pM->SetActualDataLength((long)nj.size());
                    break;
                }
            }
            q -= 10;
        }
    }
}

// --- IPC ---
static void ProcessPipeLineBuffer(std::string& buffer) {
    while (true) {
        size_t pos = buffer.find('\n'); if (pos == std::string::npos) break;
        std::string line = buffer.substr(0, pos); buffer.erase(0, pos + 1);
        try {
            auto j = json::parse(line);
            if (j.value("CMD", "") == "UnloadDLL") { DebugLog::log("[WebcamDLL][IPC] UnloadDLL requested."); g_bUnloading = true; }
            else if (j.value("CMD", "") == "UpdateMarkerDLL") {
                RenderSnapshot snap; if (TryParseSnapshotFromJson(j, snap)) RendererManager::Instance().SetSnapshot(snap);
            }
        } catch (...) {}
    }
}
static DWORD WINAPI IpcClientThread(LPVOID) {
    std::string buffer;
    while (!g_bUnloading.load()) {
        HANDLE hPipe = CreateFileW(kPipeInject, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
        char tmp[2048]; DWORD cb = 0;
        while (!g_bUnloading.load() && ReadFile(hPipe, tmp, sizeof(tmp), &cb, nullptr) && cb > 0) {
            buffer.append(tmp, cb); ProcessPipeLineBuffer(buffer);
        }
        CloseHandle(hPipe);
    }
    return 0;
}

// --- Hooks ---
typedef HRESULT(STDMETHODCALLTYPE* PReadSample)(IMFSourceReader*, DWORD, DWORD, DWORD*, DWORD*, LONGLONG*, IMFSample**);
static PReadSample g_origReadSample = NULL;
typedef HRESULT(STDMETHODCALLTYPE* POnReadSample)(IMFSourceReaderCallback*, HRESULT, DWORD, DWORD, LONGLONG, IMFSample*);
static POnReadSample g_origOnReadSample = NULL;
typedef HRESULT(STDMETHODCALLTYPE* PSetCMT)(IMFSourceReader*, DWORD, DWORD*, IMFMediaType*);
static PSetCMT g_origSetCMT = NULL;

void ProcessMFSample(void* r, IMFSample* pS, DWORD di) {
    if (!pS) return; g_activeCalls++;
    if (!g_bUnloading.load()) {
        IMFMediaBuffer* pB = NULL; if (SUCCEEDED(pS->ConvertToContiguousBuffer(&pB))) {
            BYTE* pD = NULL; DWORD maxL=0, curL=0; if (SUCCEEDED(pB->Lock(&pD, &maxL, &curL))) {
                VideoConfig c; { std::lock_guard<std::mutex> lk(g_cfgMutex); if (g_videoConfigs.count(r)) c = g_videoConfigs[r]; }
                if (c.width == 0) { c.width=640; c.height=480; }
                bool pMJ = (curL > 30 && pD[2] == 0xFF && pD[3] == 0xFE && pD[6] == 'A');
                if (c.isCompressed && !pMJ) ProcessMJPGFrame(pD, curL, maxL, pB, NULL);
                else if (!c.isCompressed && !IsRawFrameAlreadyProcessed(pD)) {
                    DWORD exp = c.isNV12 ? (c.width * c.height * 3 / 2) : (c.width * c.height * 2);
                    if (curL >= exp) {
                        int stride = c.isNV12 ? c.width : ((c.width * 2 + 15) & ~15); // Integrated cam alignment
                        ProcessWatermarkInternal(pD, c.width, c.height, c.isNV12 ? 1 : 0, stride, false);
                    }
                }
                pB->Unlock();
            }
            pB->Release();
        }
    }
    g_activeCalls--;
}

HRESULT STDMETHODCALLTYPE HookedReadSample(IMFSourceReader* pS, DWORD di, DWORD df, DWORD* ad, DWORD* sf, LONGLONG* ts, IMFSample** sa) {
    HRESULT hr = g_origReadSample(pS, di, df, ad, sf, ts, sa);
    if (SUCCEEDED(hr) && sa && *sa) ProcessMFSample(pS, *sa, (ad ? *ad : di));
    return hr;
}
HRESULT STDMETHODCALLTYPE HookedOnReadSample(IMFSourceReaderCallback* pS, HRESULT hr, DWORD di, DWORD df, LONGLONG ts, IMFSample* sa) {
    if (SUCCEEDED(hr) && sa) {
        void* pR = nullptr; { std::lock_guard<std::mutex> lk(g_mfMapMutex); if (g_mfCallbackToReader.count(pS)) pR = g_mfCallbackToReader[pS]; }
        ProcessMFSample(pR, sa, di);
    }
    return g_origOnReadSample(pS, hr, di, df, ts, sa);
}
HRESULT STDMETHODCALLTYPE HookedSetCMT(IMFSourceReader* pS, DWORD di, DWORD* pr, IMFMediaType* pT) {
    if (pT) {
        VideoConfig c; UINT32 w=0, h=0; MFGetAttributeSize(pT, MF_MT_FRAME_SIZE, &w, &h);
        if (w>0) { c.width=w; c.height=h; GUID sub; if (SUCCEEDED(pT->GetGUID(MF_MT_SUBTYPE, &sub))) {
            c.isNV12 = (sub == MFVideoFormat_NV12); c.isCompressed = (sub == MFVideoFormat_MJPG);
        }
        std::lock_guard<std::mutex> lk(g_cfgMutex); g_videoConfigs[pS] = c; }
    }
    return g_origSetCMT(pS, di, pr, pT);
}

void HookSourceReader(IMFSourceReader* pR, IMFAttributes* pA) {
    if (!pR) return; std::lock_guard<std::mutex> lk(g_hookMutex);
    void** vt = *(void***)pR;
    if (!g_bMFHooked.exchange(true)) {
        MH_CreateHook(vt[7], &HookedSetCMT, (LPVOID*)&g_origSetCMT);
        MH_CreateHook(vt[9], &HookedReadSample, (LPVOID*)&g_origReadSample);
        MH_EnableHook(vt[7]); MH_EnableHook(vt[9]);
    }
    if (pA) {
        IUnknown* pC = NULL; if (SUCCEEDED(pA->GetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, IID_IUnknown, (LPVOID*)&pC))) {
            { std::lock_guard<std::mutex> lkMap(g_mfMapMutex); g_mfCallbackToReader[pC] = pR; }
            void** vtC = *(void***)pC; if (MH_CreateHook(vtC[3], &HookedOnReadSample, (LPVOID*)&g_origOnReadSample) == MH_OK) MH_EnableHook(vtC[3]);
            pC->Release();
        }
    }
}

typedef HRESULT(WINAPI* PMFCreateSR)(IMFMediaSource*, IMFAttributes*, IMFSourceReader**);
static PMFCreateSR g_origMFCreateSR = NULL;
HRESULT WINAPI HookedMFCreateSR(IMFMediaSource* pM, IMFAttributes* pA, IMFSourceReader** pS) {
    HRESULT hr = g_origMFCreateSR(pM, pA, pS); if (SUCCEEDED(hr) && pS && *pS) HookSourceReader(*pS, pA); return hr;
}

// --- DirectShow Hooks ---
typedef HRESULT(STDMETHODCALLTYPE* PReceive)(IMemInputPin*, IMediaSample*);
static PReceive g_origReceive = NULL;
HRESULT STDMETHODCALLTYPE HookedReceive(IMemInputPin* pS, IMediaSample* pM) {
    if (pM) {
        g_activeCalls++;
        if (!g_bUnloading.load()) {
            BYTE* pB = NULL; if (SUCCEEDED(pM->GetPointer(&pB))) {
                VideoConfig c; { std::lock_guard<std::mutex> lk(g_cfgMutex); if (g_videoConfigs.count(pS)) c = g_videoConfigs[pS]; }
                if (c.width == 0) { c.width=640; c.height=480; }
                DWORD curL = (DWORD)pM->GetActualDataLength();
                bool pMJ = (curL > 30 && pB[2] == 0xFF && pB[3] == 0xFE && pB[6] == 'A');
                if (c.isCompressed && !pMJ) ProcessMJPGFrame(pB, curL, (DWORD)pM->GetSize(), NULL, pM);
                else if (!c.isCompressed && !IsRawFrameAlreadyProcessed(pB)) {
                    DWORD exp = c.isNV12 ? (c.width * c.height * 3 / 2) : (c.width * c.height * 2);
                    if (curL >= exp) {
                        int stride = c.isNV12 ? c.width : ((c.width * 2 + 15) & ~15);
                        ProcessWatermarkInternal(pB, c.width, c.height, c.isNV12 ? 1 : 0, stride, false);
                    }
                }
            }
        }
    }
    HRESULT hr = g_origReceive(pS, pM);
    if (pM) g_activeCalls--;
    return hr;
}

typedef HRESULT(STDMETHODCALLTYPE* PDSSetFormat)(IAMStreamConfig*, AM_MEDIA_TYPE*);
static PDSSetFormat g_origDSSetFormat = NULL;
HRESULT STDMETHODCALLTYPE HookedDSSetFormat(IAMStreamConfig* pS, AM_MEDIA_TYPE* pmt) {
    HRESULT hr = g_origDSSetFormat(pS, pmt);
    if (SUCCEEDED(hr) && pmt) {
        if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
            VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
            VideoConfig cfg; cfg.width = vih->bmiHeader.biWidth; cfg.height = (int)abs(vih->bmiHeader.biHeight);
            cfg.isNV12 = (pmt->subtype == MEDIASUBTYPE_NV12); cfg.isCompressed = (pmt->subtype == MEDIASUBTYPE_MJPG);
            std::lock_guard<std::mutex> lk(g_cfgMutex); for (auto& p : g_videoConfigs) { if (p.second.width == 0) p.second = cfg; }
        }
    }
    return hr;
}

typedef HRESULT(STDMETHODCALLTYPE* PGraphConnect)(IGraphBuilder*, IPin*, IPin*);
static PGraphConnect g_origGraphConnect = NULL;
HRESULT STDMETHODCALLTYPE HookedGraphConnect(IGraphBuilder* pS, IPin* pO, IPin* pI) {
    HRESULT hr = g_origGraphConnect(pS, pO, pI);
    if (SUCCEEDED(hr)) {
        IAMStreamConfig* pCfg = NULL; if (SUCCEEDED(pO->QueryInterface(IID_IAMStreamConfig, (void**)&pCfg))) {
            void** vtC = *(void***)pCfg; MH_CreateHook(vtC[3], &HookedDSSetFormat, (LPVOID*)&g_origDSSetFormat); MH_EnableHook(vtC[3]);
            pCfg->Release();
        }
        AM_MEDIA_TYPE mt; if (SUCCEEDED(pI->ConnectionMediaType(&mt))) {
            if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
                VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)mt.pbFormat;
                VideoConfig cfg; cfg.width = vih->bmiHeader.biWidth; cfg.height = (int)abs(vih->bmiHeader.biHeight);
                cfg.isNV12 = (mt.subtype == MEDIASUBTYPE_NV12); cfg.isCompressed = (mt.subtype == MEDIASUBTYPE_MJPG);
                IMemInputPin* pMip = NULL; if (SUCCEEDED(pI->QueryInterface(IID_IMemInputPin, (void**)&pMip))) {
                    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_videoConfigs[pMip] = cfg; }
                    void** vt = *(void***)pMip; if (!g_origReceive) { MH_CreateHook(vt[6], &HookedReceive, (LPVOID*)&g_origReceive); MH_EnableHook(vt[6]); }
                    pMip->Release();
                }
            }
            FreeMediaType(mt);
        }
    }
    return hr;
}

typedef HRESULT(WINAPI* PCoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static PCoCreate g_origCoCreate = NULL;
HRESULT WINAPI HookedCoCreate(REFCLSID clsid, LPUNKNOWN pU, DWORD ctx, REFIID riid, LPVOID* ppv) {
    HRESULT hr = g_origCoCreate(clsid, pU, ctx, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (riid == IID_IGraphBuilder || riid == IID_IFilterGraph) {
            void** vt = *(void***)*ppv; MH_CreateHook(vt[11], &HookedGraphConnect, (LPVOID*)&g_origGraphConnect); MH_EnableHook(vt[11]);
        }
    }
    return hr;
}

// --- Watchdog ---
DWORD WINAPI WatchdogThread(LPVOID) {
    while (!g_bUnloading.load()) {
        Sleep(3000); HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{sizeof(pe)}; bool f = false;
        if (Process32FirstW(h, &pe)) { do { if (_wcsicmp(pe.szExeFile, L"AgileMark.exe") == 0) { f = true; break; } } while (Process32NextW(h, &pe)); }
        CloseHandle(h); if (!f) { DebugLog::log("[WebcamDLL] AgileMark not found. Unloading..."); g_bUnloading = true; }
    }
    DebugLog::log("[WebcamDLL] Waiting for threads...");
    while (g_activeCalls.load() > 0) Sleep(50);
    MH_DisableHook(MH_ALL_HOOKS); Sleep(1000);
    { std::lock_guard<std::mutex> lk(g_cacheMutex); for (auto& p : g_resBmpCache) delete p.second; g_resBmpCache.clear(); }
    CoUninitialize(); DebugLog::log("[WebcamDLL] Safe to exit");
    FreeLibraryAndExitThread(g_hModule, 0); return 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI StartWatch(LPVOID lp) {
    if (g_bInitialized.exchange(true)) return 0;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DebugLog::initialize(); DebugLog::log("[WebcamDLL] StartWatch");
    wchar_t mp[MAX_PATH]; if (GetModuleFileNameW(NULL, mp, MAX_PATH)) {
        std::wstring p(mp); for (auto& c : p) c = towlower(c);
        if (p.find(L"zoom.exe") != std::wstring::npos || p.find(L"ms-teams.exe") != std::wstring::npos || p.find(L"ciscocollabhost.exe") != std::wstring::npos) g_isMirrorMode = true;
    }
    Gdiplus::GdiplusStartupInput gsi; Gdiplus::GdiplusStartup(&g_gdiplusToken, &gsi, NULL);
    CreateThread(NULL, 0, IpcClientThread, NULL, 0, NULL);
    if (MH_Initialize() == MH_OK) {
        HMODULE hO = GetModuleHandleW(L"ole32.dll"); if (hO) MH_CreateHook((void*)GetProcAddress(hO, "CoCreateInstance"), &HookedCoCreate, (LPVOID*)&g_origCoCreate);
        HMODULE hM = GetModuleHandleW(L"Mfreadwrite.dll"); if (!hM) hM = LoadLibraryW(L"Mfreadwrite.dll");
        if (hM) {
            void* p1 = (void*)GetProcAddress(hM, "MFCreateSourceReaderFromMediaSource");
            if (p1) MH_CreateHook(p1, &HookedMFCreateSR, (LPVOID*)&g_origMFCreateSR);
        }
        MH_EnableHook(MH_ALL_HOOKS);
    }
    CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
    return 0;
}
BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) { if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); g_hModule = h; } return TRUE; }
