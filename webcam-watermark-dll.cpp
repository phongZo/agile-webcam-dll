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
static std::atomic<bool> g_bInitialized(false);
static std::mutex g_hookMutex;
static ULONG_PTR g_gdiplusToken = 0;
static bool g_isMirrorMode = false;

static void CheckProcessAndSetMirrorMode() {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(NULL, path, MAX_PATH)) {
        std::wstring wsPath(path);
        std::transform(wsPath.begin(), wsPath.end(), wsPath.begin(), ::towlower);

        // Mirror for Zoom and Teams (both old and new versions)
        if (wsPath.find(L"zoom.exe") != std::wstring::npos ||
            wsPath.find(L"teams.exe") != std::wstring::npos) {
            g_isMirrorMode = true;
            DebugLog::log("[WebcamDLL] Mirror Mode ENABLED for target process.");
        }
        else {
            g_isMirrorMode = false;
            DebugLog::log("[WebcamDLL] Mirror Mode DISABLED for target process.");
        }
    }
}

struct BufferTag { DWORD timestamp; };
static std::map<void*, BufferTag> g_processedRawBuffers;
static std::mutex g_rawBufferMutex;

struct VideoConfig {
    int width = 0, height = 0;
    bool isNV12 = false, isCompressed = false;
};
static std::map<void*, VideoConfig> g_videoConfigs;
static std::mutex g_cfgMutex;
static VideoConfig g_lastKnownConfig; // Backup config

static std::map<void*, void*> g_mfCallbackToReader;
static std::mutex g_mfMapMutex;

// --- Shared Memory for Pre-rendered Bitmap ---
static HANDLE g_hWatermarkMap = NULL;
static BYTE* g_pWatermarkBuffer = NULL;
static int g_watermarkW = 0, g_watermarkH = 0;
static std::mutex g_sharedMemMutex;

static void CleanupSharedWatermark() {
    std::lock_guard<std::mutex> lock(g_sharedMemMutex);
    if (g_pWatermarkBuffer) { UnmapViewOfFile(g_pWatermarkBuffer); g_pWatermarkBuffer = NULL; }
    if (g_hWatermarkMap) { CloseHandle(g_hWatermarkMap); g_hWatermarkMap = NULL; }
    g_watermarkW = 0; g_watermarkH = 0;
}

static std::wstring Utf8ToUtf16(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &ws[0], len);
    return ws;
}

static void UpdateSharedWatermarkBuffer(const std::wstring& name, int w, int h) {
    if (name.empty() || w <= 0 || h <= 0) { 
        CleanupSharedWatermark(); 
        return; 
    }
    
    std::lock_guard<std::mutex> lock(g_sharedMemMutex);
    if (g_hWatermarkMap != NULL && g_watermarkW == w && g_watermarkH == h) return;

    if (g_pWatermarkBuffer) { UnmapViewOfFile(g_pWatermarkBuffer); g_pWatermarkBuffer = NULL; }
    if (g_hWatermarkMap) { CloseHandle(g_hWatermarkMap); g_hWatermarkMap = NULL; }

    g_hWatermarkMap = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (g_hWatermarkMap) {
        g_pWatermarkBuffer = (BYTE*)MapViewOfFile(g_hWatermarkMap, FILE_MAP_READ, 0, 0, 0);
        if (g_pWatermarkBuffer) {
            g_watermarkW = w;
            g_watermarkH = h;
            DebugLog::log("[WebcamDLL] Shared Watermark Buffer updated: " + std::to_string(w) + "x" + std::to_string(h));
        } else {
            CloseHandle(g_hWatermarkMap); g_hWatermarkMap = NULL;
        }
    }
}

static const wchar_t* kPipeInject = L"\\\\.\\pipe\\AgileMarkPipe_qaKOab5VPyK4ar4A6sfm2VZ0";

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

// --- Blending & YUV BT.709 Limited Range Helpers ---
// Y = 16 + 0.183R + 0.614G + 0.062B
// U = 128 - 0.101R - 0.339G + 0.439B
// V = 128 + 0.439R - 0.399G - 0.040B

void BlendARGBtoYUY2(BYTE* pData, int width, int height, int stride, Gdiplus::Bitmap* pBmp) {
    Gdiplus::BitmapData bd; Gdiplus::Rect rc(0, 0, pBmp->GetWidth(), pBmp->GetHeight());
    if (pBmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
        BYTE* pSrc = (BYTE*)bd.Scan0;
        int dW = (std::min)(width, (int)pBmp->GetWidth()), dH = (std::min)(height, (int)pBmp->GetHeight());
        for (int y = 0; y < dH; y++) {
            for (int x = 0; x < dW; x++) {
                int srcX = g_isMirrorMode ? (dW - 1 - x) : x;
                BYTE* pS = pSrc + (y * bd.Stride) + (srcX * 4);
                int alpha = pS[3];
                if (alpha > 0) {
                    int invA = 255 - alpha;
                    int base = y * stride + (x / 2) * 4;
                    int yP = base + (x % 2) * 2;
                    
                    // BT.709 Premultiplied YUV shifts (multiplied by 1000 for integer math)
                    int Yp = (183 * pS[2] + 614 * pS[1] + 62 * pS[0]) / 1000;
                    int Up = (-101 * pS[2] - 339 * pS[1] + 439 * pS[0]) / 1000;
                    int Vp = (439 * pS[2] - 399 * pS[1] - 40 * pS[0]) / 1000;

                    pData[yP] = (BYTE)(Yp + ((16 * alpha + pData[yP] * invA + 127) >> 8));
                    if (x % 2 == 0) { // U/V shared for 2 pixels
                        pData[base + 1] = (BYTE)(Up + ((128 * alpha + pData[base + 1] * invA + 127) >> 8));
                        pData[base + 3] = (BYTE)(Vp + ((128 * alpha + pData[base + 3] * invA + 127) >> 8));
                    }
                }
            }
        }
        pBmp->UnlockBits(&bd);
    }
}

void BlendARGBtoNV12(BYTE* pY, BYTE* pUV, int width, int height, int stride, Gdiplus::Bitmap* pBmp) {
    Gdiplus::BitmapData bd; Gdiplus::Rect rc(0, 0, pBmp->GetWidth(), pBmp->GetHeight());
    if (pBmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
        BYTE* pSrc = (BYTE*)bd.Scan0;
        int dW = (std::min)(width, (int)pBmp->GetWidth()), dH = (std::min)(height, (int)pBmp->GetHeight());
        for (int y = 0; y < dH; y++) {
            for (int x = 0; x < dW; x++) {
                int srcX = g_isMirrorMode ? (dW - 1 - x) : x;
                BYTE* pS = pSrc + (y * bd.Stride) + (srcX * 4);
                int alpha = pS[3];
                if (alpha > 0) {
                    int invA = 255 - alpha;
                    int yPos = y * stride + x;
                    
                    int Yp = (183 * pS[2] + 614 * pS[1] + 62 * pS[0]) / 1000;
                    pY[yPos] = (BYTE)(Yp + ((16 * alpha + pY[yPos] * invA + 127) >> 8));

                    if (y % 2 == 0 && x % 2 == 0) {
                        int uvIdx = (y / 2) * stride + (x / 2) * 2;
                        int Up = (-101 * pS[2] - 339 * pS[1] + 439 * pS[0]) / 1000;
                        int Vp = (439 * pS[2] - 399 * pS[1] - 40 * pS[0]) / 1000;
                        pUV[uvIdx] = (BYTE)(Up + ((128 * alpha + pUV[uvIdx] * invA + 127) >> 8));
                        pUV[uvIdx + 1] = (BYTE)(Vp + ((128 * alpha + pUV[uvIdx + 1] * invA + 127) >> 8));
                    }
                }
            }
        }
        pBmp->UnlockBits(&bd);
    }
}

void BlendARGBtoBGRA(BYTE* pData, int width, int height, int stride, Gdiplus::Bitmap* pBmp) {
    Gdiplus::BitmapData bd; Gdiplus::Rect rc(0, 0, pBmp->GetWidth(), pBmp->GetHeight());
    if (pBmp->LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
        BYTE* pSrc = (BYTE*)bd.Scan0;
        int dW = (std::min)(width, (int)pBmp->GetWidth()), dH = (std::min)(height, (int)pBmp->GetHeight());
        int bpp = stride / width;
        for (int y = 0; y < dH; y++) {
            for (int x = 0; x < dW; x++) {
                int srcX = g_isMirrorMode ? (dW - 1 - x) : x;
                BYTE* pS = pSrc + (y * bd.Stride) + (srcX * 4);
                int alpha = pS[3];
                if (alpha > 0) {
                    BYTE* pD = pData + (y * stride) + (x * bpp);
                    int invA = 255 - alpha;
                    // Standard Premultiplied Blend: Dest = Source + Dest * (1 - Alpha)
                    pD[0] = (BYTE)(pS[0] + ((pD[0] * invA + 127) >> 8)); // B
                    pD[1] = (BYTE)(pS[1] + ((pD[1] * invA + 127) >> 8)); // G
                    pD[2] = (BYTE)(pS[2] + ((pD[2] * invA + 127) >> 8)); // R
                    if (bpp == 4) pD[3] = 255;
                }
            }
        }
        pBmp->UnlockBits(&bd);
    }
}

static void ProcessWatermarkInternal(BYTE* pData, int width, int height, int formatType, int stride, bool isCompressed) {
    if (isCompressed || !pData || width <= 0 || height <= 0) return;
    std::lock_guard<std::mutex> shmLock(g_sharedMemMutex);
    if (g_pWatermarkBuffer && g_watermarkW > 0 && g_watermarkH > 0) {
        // Source is PixelFormats.Pbgra32 (Premultiplied)
        Gdiplus::Bitmap* pSrcBmp = new Gdiplus::Bitmap(g_watermarkW, g_watermarkH, g_watermarkW * 4, PixelFormat32bppPARGB, g_pWatermarkBuffer);
        if (pSrcBmp) {
            Gdiplus::Bitmap* pTargetBmp = pSrcBmp;
            bool needDeleteTarget = false;

            if (g_watermarkW != width || g_watermarkH != height) {
                pTargetBmp = new Gdiplus::Bitmap(width, height, PixelFormat32bppPARGB);
                if (pTargetBmp) {
                    Gdiplus::Graphics g(pTargetBmp);
                    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                    g.Clear(Gdiplus::Color(0, 0, 0, 0)); 

                    float srcAspect = (float)g_watermarkW / g_watermarkH;
                    float dstAspect = (float)width / height;

                    int drawW, drawH, drawX, drawY;
                    if (srcAspect > dstAspect) {
                        drawW = width;
                        drawH = (int)(width / srcAspect);
                        drawX = 0;
                        drawY = (height - drawH) / 2;
                    } else {
                        drawH = height;
                        drawW = (int)(height * srcAspect);
                        drawX = (width - drawW) / 2;
                        drawY = 0;
                    }
                    g.DrawImage(pSrcBmp, drawX, drawY, drawW, drawH);
                    needDeleteTarget = true;
                } else {
                    pTargetBmp = pSrcBmp;
                }
            }

            if (formatType == 1) BlendARGBtoNV12(pData, pData + (stride * height), width, height, stride, pTargetBmp);
            else if (formatType == 0) BlendARGBtoYUY2(pData, width, height, stride, pTargetBmp);
            else if (formatType == 2) BlendARGBtoBGRA(pData, width, height, stride, pTargetBmp);

            if (needDeleteTarget) delete pTargetBmp;
            delete pSrcBmp;
        }
    }
}

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
            std::string cmd = j.value("CMD", "");
            if (cmd == "UpdateBitmapShared") {
                std::string shmName = j.value("SharedMemoryName", "");
                int w = j.value("Width", 0);
                int h = j.value("Height", 0);
                UpdateSharedWatermarkBuffer(Utf8ToUtf16(shmName), w, h);
            }
        } catch (...) {}
    }
}
static DWORD WINAPI IpcClientThread(LPVOID) {
    std::string buffer;
    while (true) {
        HANDLE hPipe = CreateFileW(kPipeInject, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
        DebugLog::log("[WebcamDLL] AgileMark connected.");
        char tmp[2048]; DWORD cb = 0;
        while (ReadFile(hPipe, tmp, sizeof(tmp), &cb, nullptr) && cb > 0) {
            buffer.append(tmp, cb); ProcessPipeLineBuffer(buffer);
        }
        DebugLog::log("[WebcamDLL] AgileMark disconnected. Stopping watermark.");
        CleanupSharedWatermark();
        CloseHandle(hPipe);
    }
    return 0;
}

// --- Hooks ---
typedef HRESULT(STDMETHODCALLTYPE* PReadSample)(IMFSourceReader*, DWORD, DWORD, DWORD*, DWORD*, LONGLONG*, IMFSample**);
typedef HRESULT(STDMETHODCALLTYPE* POnReadSample)(IMFSourceReaderCallback*, HRESULT, DWORD, DWORD, LONGLONG, IMFSample*);
typedef HRESULT(STDMETHODCALLTYPE* PSetCMT)(IMFSourceReader*, DWORD, DWORD*, IMFMediaType*);
typedef HRESULT(STDMETHODCALLTYPE* PReceive)(IMemInputPin*, IMediaSample*);
typedef HRESULT(STDMETHODCALLTYPE* PDSSetFormat)(IAMStreamConfig*, AM_MEDIA_TYPE*);
typedef HRESULT(STDMETHODCALLTYPE* PGraphConnect)(IGraphBuilder*, IPin*, IPin*);

static std::map<void**, PReadSample> g_origReadSampleMap;
static std::map<void**, POnReadSample> g_origOnReadSampleMap;
static std::map<void**, PSetCMT> g_origSetCMTMap;
static std::map<void**, PReceive> g_origReceiveMap;
static std::map<void**, PDSSetFormat> g_origDSSetFormatMap;
static std::map<void**, PGraphConnect> g_origGraphConnectMap;

template<typename T>
void PatchVTable(void* pInterface, int index, void* pHookFunc, std::map<void**, T>& origMap) {
    if (!pInterface) return;
    void** vt = *(void***)pInterface;
    std::lock_guard<std::mutex> lk(g_hookMutex);
    if (vt[index] == pHookFunc) return;
    if (origMap.find(vt) == origMap.end()) origMap[vt] = (T)vt[index];
    DWORD old;
    if (VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        vt[index] = pHookFunc;
        VirtualProtect(&vt[index], sizeof(void*), old, &old);
    }
}

void ProcessMFSample(void* r, IMFSample* pS, DWORD di) {
    if (!pS) return;
    IMFMediaBuffer* pB = NULL; if (SUCCEEDED(pS->ConvertToContiguousBuffer(&pB))) {
        BYTE* pD = NULL; DWORD maxL=0, curL=0; if (SUCCEEDED(pB->Lock(&pD, &maxL, &curL))) {
            VideoConfig c; { 
                std::lock_guard<std::mutex> lk(g_cfgMutex); 
                if (g_videoConfigs.count(r)) {
                    c = g_videoConfigs[r]; 
                    g_lastKnownConfig = c;
                } else {
                    c = g_lastKnownConfig;
                }
            }
            if (c.width == 0) { c.width=640; c.height=480; }
            bool pMJ = (curL > 30 && pD[2] == 0xFF && pD[3] == 0xFE && pD[6] == 'A');
            if (c.isCompressed && !pMJ) ProcessMJPGFrame(pD, curL, maxL, pB, NULL);
            else if (!c.isCompressed && !IsRawFrameAlreadyProcessed(pD)) {
                DWORD exp = c.isNV12 ? (c.width * c.height * 3 / 2) : (c.width * c.height * 2);
                if (curL >= exp) {
                    int stride = c.isNV12 ? c.width : ((c.width * 2 + 15) & ~15);
                    ProcessWatermarkInternal(pD, c.width, c.height, c.isNV12 ? 1 : 0, stride, false);
                }
            }
            pB->Unlock();
        }
        pB->Release();
    }
}

HRESULT STDMETHODCALLTYPE HookedReadSample(IMFSourceReader* pS, DWORD di, DWORD df, DWORD* ad, DWORD* sf, LONGLONG* ts, IMFSample** sa) {
    void** vt = *(void***)pS; PReadSample orig = nullptr;
    { std::lock_guard<std::mutex> lk(g_hookMutex); auto it = g_origReadSampleMap.find(vt); if (it != g_origReadSampleMap.end()) orig = it->second; }
    HRESULT hr = orig ? orig(pS, di, df, ad, sf, ts, sa) : E_FAIL;
    if (SUCCEEDED(hr) && sa && *sa) ProcessMFSample(pS, *sa, (ad ? *ad : di));
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedOnReadSample(IMFSourceReaderCallback* pS, HRESULT hr, DWORD di, DWORD df, LONGLONG ts, IMFSample* sa) {
    if (SUCCEEDED(hr) && sa) {
        void* pR = nullptr; { std::lock_guard<std::mutex> lk(g_mfMapMutex); if (g_mfCallbackToReader.count(pS)) pR = g_mfCallbackToReader[pS]; }
        ProcessMFSample(pR, sa, di);
    }
    void** vt = *(void***)pS; POnReadSample orig = nullptr;
    { std::lock_guard<std::mutex> lk(g_hookMutex); auto it = g_origOnReadSampleMap.find(vt); if (it != g_origOnReadSampleMap.end()) orig = it->second; }
    return orig ? orig(pS, hr, di, df, ts, sa) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE HookedSetCMT(IMFSourceReader* pS, DWORD di, DWORD* pr, IMFMediaType* pT) {
    if (pT) {
        VideoConfig c; UINT32 w=0, h=0; MFGetAttributeSize(pT, MF_MT_FRAME_SIZE, &w, &h);
        if (w>0) { 
            c.width=w; c.height=h; 
            GUID sub; if (SUCCEEDED(pT->GetGUID(MF_MT_SUBTYPE, &sub))) {
                c.isNV12 = (sub == MFVideoFormat_NV12); c.isCompressed = (sub == MFVideoFormat_MJPG);
            }
            DebugLog::log("[WebcamDLL] MF Resolution Updated: " + std::to_string(w) + "x" + std::to_string(h));
            std::lock_guard<std::mutex> lk(g_cfgMutex); 
            g_videoConfigs[pS] = c; 
            g_lastKnownConfig = c;
        }
    }
    void** vt = *(void***)pS; PSetCMT orig = nullptr;
    { std::lock_guard<std::mutex> lk(g_hookMutex); auto it = g_origSetCMTMap.find(vt); if (it != g_origSetCMTMap.end()) orig = it->second; }
    return orig ? orig(pS, di, pr, pT) : E_FAIL;
}

void HookSourceReader(IMFSourceReader* pR, IMFAttributes* pA) {
    if (!pR) return;
    PatchVTable(pR, 7, (void*)&HookedSetCMT, g_origSetCMTMap);
    PatchVTable(pR, 9, (void*)&HookedReadSample, g_origReadSampleMap);
    if (pA) {
        IUnknown* pC = NULL; if (SUCCEEDED(pA->GetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, IID_IUnknown, (LPVOID*)&pC))) {
            { std::lock_guard<std::mutex> lkMap(g_mfMapMutex); g_mfCallbackToReader[pC] = pR; }
            PatchVTable(pC, 3, (void*)&HookedOnReadSample, g_origOnReadSampleMap);
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
HRESULT STDMETHODCALLTYPE HookedReceive(IMemInputPin* pS, IMediaSample* pM) {
    if (pM) {
        BYTE* pB = NULL; if (SUCCEEDED(pM->GetPointer(&pB))) {
            VideoConfig c; { 
                std::lock_guard<std::mutex> lk(g_cfgMutex); 
                if (g_videoConfigs.count(pS)) {
                    c = g_videoConfigs[pS]; 
                    g_lastKnownConfig = c;
                } else {
                    c = g_lastKnownConfig;
                }
            }
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
    void** vt = *(void***)pS; PReceive orig = nullptr;
    { std::lock_guard<std::mutex> lk(g_hookMutex); auto it = g_origReceiveMap.find(vt); if (it != g_origReceiveMap.end()) orig = it->second; }
    return orig ? orig(pS, pM) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE HookedDSSetFormat(IAMStreamConfig* pS, AM_MEDIA_TYPE* pmt) {
    void** vt = *(void***)pS; PDSSetFormat orig = nullptr;
    { std::lock_guard<std::mutex> lk(g_hookMutex); auto it = g_origDSSetFormatMap.find(vt); if (it != g_origDSSetFormatMap.end()) orig = it->second; }
    HRESULT hr = orig ? orig(pS, pmt) : E_FAIL;
    if (SUCCEEDED(hr) && pmt) {
        if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
            VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
            VideoConfig cfg; cfg.width = vih->bmiHeader.biWidth; cfg.height = (int)abs(vih->bmiHeader.biHeight);
            cfg.isNV12 = (pmt->subtype == MEDIASUBTYPE_NV12); cfg.isCompressed = (pmt->subtype == MEDIASUBTYPE_MJPG);
            DebugLog::log("[WebcamDLL] DS Resolution Updated: " + std::to_string(cfg.width) + "x" + std::to_string(cfg.height));
            std::lock_guard<std::mutex> lk(g_cfgMutex); 
            g_videoConfigs[pS] = cfg;
            g_lastKnownConfig = cfg;
            for (auto& p : g_videoConfigs) { if (p.second.width == 0) p.second = cfg; }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedGraphConnect(IGraphBuilder* pS, IPin* pO, IPin* pI) {
    void** vt = *(void***)pS; PGraphConnect orig = nullptr;
    { std::lock_guard<std::mutex> lk(g_hookMutex); auto it = g_origGraphConnectMap.find(vt); if (it != g_origGraphConnectMap.end()) orig = it->second; }
    HRESULT hr = orig ? orig(pS, pO, pI) : E_FAIL;
    if (SUCCEEDED(hr)) {
        IAMStreamConfig* pCfg = NULL; if (SUCCEEDED(pO->QueryInterface(IID_IAMStreamConfig, (void**)&pCfg))) {
            PatchVTable(pCfg, 3, (void*)&HookedDSSetFormat, g_origDSSetFormatMap);
            pCfg->Release();
        }
        AM_MEDIA_TYPE mt; if (SUCCEEDED(pI->ConnectionMediaType(&mt))) {
            if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER)) {
                VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)mt.pbFormat;
                VideoConfig cfg; cfg.width = vih->bmiHeader.biWidth; cfg.height = (int)abs(vih->bmiHeader.biHeight);
                cfg.isNV12 = (mt.subtype == MEDIASUBTYPE_NV12); cfg.isCompressed = (mt.subtype == MEDIASUBTYPE_MJPG);
                IMemInputPin* pMip = NULL; if (SUCCEEDED(pI->QueryInterface(IID_IMemInputPin, (void**)&pMip))) {
                    { std::lock_guard<std::mutex> lk(g_cfgMutex); g_videoConfigs[pMip] = cfg; }
                    PatchVTable(pMip, 6, (void*)&HookedReceive, g_origReceiveMap);
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
            PatchVTable(*ppv, 11, &HookedGraphConnect, g_origGraphConnectMap);
        }
    }
    return hr;
}

extern "C" __declspec(dllexport) DWORD WINAPI StartWatch(LPVOID lp) {
    if (g_bInitialized.exchange(true)) return 0;
    CheckProcessAndSetMirrorMode();
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
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
    return 0;
}
BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) { if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); g_hModule = h; } return TRUE; }
