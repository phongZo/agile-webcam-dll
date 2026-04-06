#include "pch.h"
#include <windows.h>
#include <string>
#include <atomic>
#include <mutex>
#include <map>
#include <dshow.h>
#include <amvideo.h> // VIDEOINFOHEADER / VIDEOINFOHEADER2 (FORMAT_VideoInfo / FORMAT_VideoInfo2)
#include <dvdmedia.h> // VIDEOINFOHEADER2 definition on newer SDKs
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
#include "packages/minhook.1.3.3/lib/native/include/MinHook.h"
#include "packages/nlohmann.json.3.12.0/build/native/include/nlohmann/json.hpp"
#include "JpegHelper.h"

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "quartz.lib") /* DirectShow base (Filter Graph); pairs with strmiids for some link scenarios */
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")

using json = nlohmann::json;

#ifdef _WIN64
    #pragma comment(lib, "packages/minhook.1.3.3/lib/native/lib/libMinHook-x64-v141-mt.lib")
#else
    #pragma comment(lib, "packages/minhook.1.3.3/lib/native/lib/libMinHook-x86-v141-mt.lib")
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
// Some camera stacks (Chrome/WebRTC) use planar 4:2:0 (I420/IYUV/YV12).
// Do NOT rely on SDK-provided MEDIASUBTYPE_* symbols: some kits declare them but don't link them.
static const GUID kSub_I420 = { 0x30323449, 0x0000, 0x0010, { 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71 } }; // 'I420'
static const GUID kSub_IYUV = { 0x56555949, 0x0000, 0x0010, { 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71 } }; // 'IYUV'
static const GUID kSub_YV12 = { 0x32315659, 0x0000, 0x0010, { 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71 } }; // 'YV12'

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
    bool isNV12 = false, isI420 = false, isCompressed = false;
};
static std::map<void*, VideoConfig> g_videoConfigs;
static std::mutex g_cfgMutex;
static VideoConfig g_lastKnownConfig; // Backup config

/* Async MF: OnReadSample's "this" is IMFSourceReaderCallback* — map must use that pointer, not IUnknown* (COM identity). */
static std::map<IMFSourceReaderCallback*, IMFSourceReader*> g_mfCallbackToReader;
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

// Forward declaration: used by WatermarkI420InPlace helper.
static void ProcessWatermarkInternal(BYTE* pData, int width, int height, int formatType, int stride, bool isCompressed);

static bool IsPlanar420Subtype(const GUID& sub) {
    return (sub == kSub_I420 || sub == kSub_IYUV || sub == kSub_YV12 ||
            sub == MFVideoFormat_I420 || sub == MFVideoFormat_IYUV || sub == MFVideoFormat_YV12);
}

/*
 * Chrome/WebRTC commonly delivers I420 (planar 4:2:0) frames. Our drawing core supports NV12/YUY2/BGRA.
 * We preserve the caller's format by converting I420 -> NV12, drawing in NV12, then converting back.
 *
 * Layout assumptions (contiguous, stride == width):
 *   Y plane:  width*height
 *   U plane: (width/2)*(height/2)
 *   V plane: (width/2)*(height/2)
 */
static bool WatermarkI420InPlace(BYTE* pData, int width, int height) {
    if (!pData || width <= 0 || height <= 0) return false;
    const int w2 = width / 2;
    const int h2 = height / 2;
    if (w2 <= 0 || h2 <= 0) return false;
    const size_t ySz = (size_t)width * (size_t)height;
    const size_t uSz = (size_t)w2 * (size_t)h2;
    const size_t vSz = uSz;
    const size_t total = ySz + uSz + vSz;

    std::vector<BYTE> nv12(total);
    BYTE* y = pData;
    BYTE* u = pData + ySz;
    BYTE* v = pData + ySz + uSz;
    BYTE* ny = nv12.data();
    BYTE* nuv = nv12.data() + ySz;

    memcpy(ny, y, ySz);
    for (int j = 0; j < h2; ++j) {
        for (int i = 0; i < w2; ++i) {
            const size_t idx = (size_t)j * (size_t)w2 + (size_t)i;
            nuv[idx * 2 + 0] = u[idx];
            nuv[idx * 2 + 1] = v[idx];
        }
    }

    ProcessWatermarkInternal(nv12.data(), width, height, 1, width, false);

    memcpy(y, ny, ySz);
    for (int j = 0; j < h2; ++j) {
        for (int i = 0; i < w2; ++i) {
            const size_t idx = (size_t)j * (size_t)w2 + (size_t)i;
            u[idx] = nuv[idx * 2 + 0];
            v[idx] = nuv[idx * 2 + 1];
        }
    }
    return true;
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
        for (;;) {
            BOOL ok = ReadFile(hPipe, tmp, sizeof(tmp), &cb, nullptr);
            if (!ok) {
                DWORD err = GetLastError();
                DebugLog::log("[WebcamDLL] Pipe read failed. Win32=" + std::to_string((unsigned long)err));
                break;
            }
            if (cb == 0)
                break;
            buffer.append(tmp, cb);
            ProcessPipeLineBuffer(buffer);
        }
        DebugLog::log("[WebcamDLL] AgileMark disconnected (will retry connect).");
        // Do not clear SHM here: browsers recycle processes and the host may briefly tear the pipe;
        // clearing makes the watermark flash off until the next frame path runs. Server pushes fresh SHM on reconnect.
        CloseHandle(hPipe);
        Sleep(500);
    }
    return 0;
}

// =============================================================================
// Hooks — vtable slot indices (empirically aligned with Windows SDK + QWC)
// =============================================================================
// These are 0-based indices into the interface vtable after the three IUnknown entries.
// Do not "fix" them from a header guess without testing — wrong slot = silent no-hook or crash.
//
// MF (mfreadwrite.idl): IMFSourceReader — SetCurrentMediaType = 7, ReadSample = 9.
// IMFSourceReaderCallback — OnReadSample = 3 (async delivery; sync ReadSample may not run).
//
// DirectShow Filter Graph (same concrete object): IGraphBuilder::Connect = 11,
// IFilterGraph::ConnectDirect = 7. Chromium / Edge often prefer ConnectDirect.
//
// Pin side: IMemInputPin::Receive = 6 (compressed/uncompressed samples), IAMStreamConfig::SetFormat = 3.
//
// Canonical reference in repo: QWC qwcd/dllmain.cpp (IGRAPHBUILDER_CONNECT_SLOT / IFILTERGRAPH_*).
// =============================================================================

static const int kSlot_IMFSourceReader_SetCurrentMediaType = 7;
static const int kSlot_IMFSourceReader_ReadSample = 9;
static const int kSlot_IMFSourceReaderCallback_OnReadSample = 3;
static const int kSlot_IGraphBuilder_Connect = 11;
static const int kSlot_IFilterGraph_ConnectDirect = 7;
static const int kSlot_IMemInputPin_Receive = 6;
static const int kSlot_IAMStreamConfig_SetFormat = 3;

typedef HRESULT(STDMETHODCALLTYPE* PReadSample)(IMFSourceReader*, DWORD, DWORD, DWORD*, DWORD*, LONGLONG*, IMFSample**);
typedef HRESULT(STDMETHODCALLTYPE* POnReadSample)(IMFSourceReaderCallback*, HRESULT, DWORD, DWORD, LONGLONG, IMFSample*);
typedef HRESULT(STDMETHODCALLTYPE* PSetCMT)(IMFSourceReader*, DWORD, DWORD*, IMFMediaType*);
typedef HRESULT(STDMETHODCALLTYPE* PReceive)(IMemInputPin*, IMediaSample*);
typedef HRESULT(STDMETHODCALLTYPE* PDSSetFormat)(IAMStreamConfig*, AM_MEDIA_TYPE*);
typedef HRESULT(STDMETHODCALLTYPE* PGraphConnect)(IGraphBuilder*, IPin*, IPin*);
typedef HRESULT(STDMETHODCALLTYPE* PConnectDirect)(IFilterGraph*, IPin*, IPin*);

static std::map<void**, PReadSample> g_origReadSampleMap;
static std::map<void**, POnReadSample> g_origOnReadSampleMap;
static std::map<void**, PSetCMT> g_origSetCMTMap;
static std::map<void**, PReceive> g_origReceiveMap;
static std::map<void**, PDSSetFormat> g_origDSSetFormatMap;
static std::map<void**, PGraphConnect> g_origGraphConnectMap;
static std::map<void**, PConnectDirect> g_origConnectDirectMap;

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

/* Legacy MF path: uses g_videoConfigs[reader] / g_lastKnownConfig only (Teams/Zoom style). */
static void ProcessMFSampleLegacy(IMFSourceReader* pReader, IMFSample* pSample) {
    if (!pSample) return;
    void* cfgKey = (void*)pReader;
    IMFMediaBuffer* pB = nullptr;
    if (FAILED(pSample->ConvertToContiguousBuffer(&pB)) || !pB) {
        DWORD bufCount = 0;
        if (FAILED(pSample->GetBufferCount(&bufCount)) || bufCount == 0) return;
        if (FAILED(pSample->GetBufferByIndex(0, &pB)) || !pB) return;
    }
    BYTE* pD = nullptr;
    DWORD maxL = 0, curL = 0;
    if (FAILED(pB->Lock(&pD, &maxL, &curL)) || !pD) {
        pB->Release();
        return;
    }
    VideoConfig c;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        if (pReader && g_videoConfigs.count(cfgKey)) {
            c = g_videoConfigs[cfgKey];
            g_lastKnownConfig = c;
        } else {
            c = g_lastKnownConfig;
        }
    }
    if (c.width == 0) { c.width = 640; c.height = 480; }
    bool pMJ = (curL > 30 && pD[2] == 0xFF && pD[3] == 0xFE && pD[6] == 'A');
    if (c.isCompressed && !pMJ) ProcessMJPGFrame(pD, curL, maxL, pB, NULL);
    else if (!c.isCompressed && !IsRawFrameAlreadyProcessed(pD)) {
        const bool planar420 = c.isI420;
        DWORD exp = (c.isNV12 || planar420) ? (c.width * c.height * 3 / 2) : (c.width * c.height * 2);
        if (curL >= exp) {
            if (planar420) {
                WatermarkI420InPlace(pD, c.width, c.height);
            } else {
                int stride = c.isNV12 ? c.width : ((c.width * 2 + 15) & ~15);
                ProcessWatermarkInternal(pD, c.width, c.height, c.isNV12 ? 1 : 0, stride, false);
            }
        }
    }
    pB->Unlock();
    pB->Release();
}

/*
 * Browser-oriented MF sample processing (aligned with QWC):
 * - Ignores MF_SOURCE_READERF_STREAM_TICK (no pixel payload).
 * - Resolves IMFMediaType for the stream; requires MFMediaType_Video so we do not draw on audio samples.
 * - Updates g_videoConfigs[reader] from the type when possible.
 * - Buffer: ConvertToContiguousBuffer, else GetBufferByIndex(0).
 * Then runs the same agile drawing as legacy (ProcessWatermarkInternal / MJPEG).
 */
static void ProcessMFSample(IMFSourceReader* pReader, IMFSample* pSample, DWORD streamIndex, const DWORD* pdwStreamFlags) {
    if (!pSample) return;
    if (pdwStreamFlags && (*pdwStreamFlags & MF_SOURCE_READERF_STREAMTICK))
        return;

    DWORD streamForType = streamIndex;
    if (streamForType == (DWORD)MF_SOURCE_READER_ANY_STREAM ||
        streamForType == (DWORD)MF_SOURCE_READER_INVALID_STREAM_INDEX)
        streamForType = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;

    IMFMediaType* pType = nullptr;
    HRESULT hrT = E_FAIL;
    if (pReader)
        hrT = pReader->GetCurrentMediaType(streamForType, &pType);
    if ((FAILED(hrT) || !pType) && pReader) {
        if (pType) { pType->Release(); pType = nullptr; }
        hrT = pReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pType);
    }

    if (FAILED(hrT) || !pType) {
        ProcessMFSampleLegacy(pReader, pSample);
        return;
    }

    GUID major = {};
    if (FAILED(pType->GetGUID(MF_MT_MAJOR_TYPE, &major)) || major != MFMediaType_Video) {
        pType->Release();
        return;
    }

    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) {
        pType->Release();
        ProcessMFSampleLegacy(pReader, pSample);
        return;
    }

    VideoConfig c;
    c.width = (int)w;
    c.height = (int)h;
    c.isNV12 = false;
    c.isI420 = false;
    c.isCompressed = false;
    GUID sub = {};
    if (SUCCEEDED(pType->GetGUID(MF_MT_SUBTYPE, &sub))) {
        c.isNV12 = (sub == MFVideoFormat_NV12);
        c.isI420 = IsPlanar420Subtype(sub);
        c.isCompressed = (sub == MFVideoFormat_MJPG);
    }

    UINT32 strideAttr = 0;
    const bool haveStride = SUCCEEDED(pType->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideAttr)) && strideAttr > 0;
    pType->Release();

    if (pReader) {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_videoConfigs[(void*)pReader] = c;
        g_lastKnownConfig = c;
    }

    IMFMediaBuffer* pB = nullptr;
    if (FAILED(pSample->ConvertToContiguousBuffer(&pB)) || !pB) {
        DWORD bufCount = 0;
        if (FAILED(pSample->GetBufferCount(&bufCount)) || bufCount == 0) return;
        if (FAILED(pSample->GetBufferByIndex(0, &pB)) || !pB) return;
    }

    BYTE* pD = nullptr;
    DWORD maxL = 0, curL = 0;
    if (FAILED(pB->Lock(&pD, &maxL, &curL)) || !pD) {
        pB->Release();
        return;
    }

    if (c.width == 0) { c.width = 640; c.height = 480; }
    int stride = (c.isNV12 || c.isI420) ? c.width : ((c.width * 2 + 15) & ~15);
    if (!(c.isNV12 || c.isI420) && haveStride)
        stride = (int)strideAttr;

    bool pMJ = (curL > 30 && pD[2] == 0xFF && pD[3] == 0xFE && pD[6] == 'A');
    if (c.isCompressed && !pMJ) ProcessMJPGFrame(pD, curL, maxL, pB, NULL);
    else if (!c.isCompressed && !IsRawFrameAlreadyProcessed(pD)) {
        const bool planar420 = c.isI420;
        DWORD exp = (c.isNV12 || planar420) ? (c.width * c.height * 3 / 2) : (c.width * c.height * 2);
        if (curL >= exp) {
            if (planar420) WatermarkI420InPlace(pD, c.width, c.height);
            else ProcessWatermarkInternal(pD, c.width, c.height, c.isNV12 ? 1 : 0, stride, false);
        }
    }

    pB->Unlock();
    pB->Release();
}

HRESULT STDMETHODCALLTYPE HookedReadSample(IMFSourceReader* pS, DWORD di, DWORD df, DWORD* ad, DWORD* sf, LONGLONG* ts, IMFSample** sa) {
    void** vt = *(void***)pS;
    PReadSample orig = nullptr;
    { std::lock_guard<std::mutex> lk(g_hookMutex); auto it = g_origReadSampleMap.find(vt); if (it != g_origReadSampleMap.end()) orig = it->second; }
    HRESULT hr = orig ? orig(pS, di, df, ad, sf, ts, sa) : E_FAIL;
    if (SUCCEEDED(hr) && sa && *sa) {
        DWORD si = di;
        if (ad && *ad != (DWORD)MF_SOURCE_READER_INVALID_STREAM_INDEX)
            si = *ad;
        ProcessMFSample(pS, *sa, si, sf);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedOnReadSample(IMFSourceReaderCallback* pS, HRESULT hr, DWORD di, DWORD df, LONGLONG ts, IMFSample* sa) {
    /* Draw before the app's callback runs so downstream (WebRTC) sees the watermark. */
    if (SUCCEEDED(hr) && sa) {
        IMFSourceReader* pR = nullptr;
        { std::lock_guard<std::mutex> lk(g_mfMapMutex); auto it = g_mfCallbackToReader.find(pS); if (it != g_mfCallbackToReader.end()) pR = it->second; }
        ProcessMFSample(pR, sa, di, &df);
    }
    void** vt = *(void***)pS;
    POnReadSample orig = nullptr;
    { std::lock_guard<std::mutex> lk(g_hookMutex); auto it = g_origOnReadSampleMap.find(vt); if (it != g_origOnReadSampleMap.end()) orig = it->second; }
    return orig ? orig(pS, hr, di, df, ts, sa) : E_FAIL;
}

HRESULT STDMETHODCALLTYPE HookedSetCMT(IMFSourceReader* pS, DWORD di, DWORD* pr, IMFMediaType* pT) {
    if (pT) {
        VideoConfig c;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(pT, MF_MT_FRAME_SIZE, &w, &h);
        if (w > 0) {
            c.width = (int)w;
            c.height = (int)h;
            GUID sub;
            if (SUCCEEDED(pT->GetGUID(MF_MT_SUBTYPE, &sub))) {
                c.isNV12 = (sub == MFVideoFormat_NV12);
                c.isI420 = IsPlanar420Subtype(sub);
                c.isCompressed = (sub == MFVideoFormat_MJPG);
            }
            DebugLog::log("[WebcamDLL] MF Resolution Updated: " + std::to_string(w) + "x" + std::to_string(h));
            std::lock_guard<std::mutex> lk(g_cfgMutex);
            g_videoConfigs[(void*)pS] = c;
            g_lastKnownConfig = c;
        }
    }
    void** vt = *(void***)pS;
    PSetCMT orig = nullptr;
    { std::lock_guard<std::mutex> lk(g_hookMutex); auto it = g_origSetCMTMap.find(vt); if (it != g_origSetCMTMap.end()) orig = it->second; }
    return orig ? orig(pS, di, pr, pT) : E_FAIL;
}

void HookSourceReader(IMFSourceReader* pR, IMFAttributes* pA) {
    if (!pR) return;
    PatchVTable(pR, kSlot_IMFSourceReader_SetCurrentMediaType, (void*)&HookedSetCMT, g_origSetCMTMap);
    PatchVTable(pR, kSlot_IMFSourceReader_ReadSample, (void*)&HookedReadSample, g_origReadSampleMap);
    if (pA) {
        IUnknown* pUnkCb = nullptr;
        if (SUCCEEDED(pA->GetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, IID_IUnknown, (LPVOID*)&pUnkCb)) && pUnkCb) {
            IMFSourceReaderCallback* pCb = nullptr;
            if (SUCCEEDED(pUnkCb->QueryInterface(IID_IMFSourceReaderCallback, (void**)&pCb)) && pCb) {
                { std::lock_guard<std::mutex> lkMap(g_mfMapMutex); g_mfCallbackToReader[pCb] = pR; }
                PatchVTable(pCb, kSlot_IMFSourceReaderCallback_OnReadSample, (void*)&HookedOnReadSample, g_origOnReadSampleMap);
                pCb->Release();
            }
            pUnkCb->Release();
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
                const bool planar420 = c.isI420;
                DWORD exp = (c.isNV12 || planar420) ? (c.width * c.height * 3 / 2) : (c.width * c.height * 2);
                if (curL >= exp) {
                    if (planar420) {
                        WatermarkI420InPlace(pB, c.width, c.height);
                    } else {
                        int stride = c.isNV12 ? c.width : ((c.width * 2 + 15) & ~15);
                        ProcessWatermarkInternal(pB, c.width, c.height, c.isNV12 ? 1 : 0, stride, false);
                    }
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
        // Chrome/Edge frequently negotiate FORMAT_VideoInfo2; handle both.
        const BITMAPINFOHEADER* bih = nullptr;
        if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER))
            bih = &((VIDEOINFOHEADER*)pmt->pbFormat)->bmiHeader;
        else if (pmt->formattype == FORMAT_VideoInfo2 && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2))
            bih = &((VIDEOINFOHEADER2*)pmt->pbFormat)->bmiHeader;

        if (bih) {
            VideoConfig cfg; cfg.width = bih->biWidth; cfg.height = (int)abs(bih->biHeight);
            cfg.isNV12 = (pmt->subtype == MEDIASUBTYPE_NV12);
            cfg.isI420 = IsPlanar420Subtype(pmt->subtype);
            cfg.isCompressed = (pmt->subtype == MEDIASUBTYPE_MJPG);
            DebugLog::log("[WebcamDLL] DS Resolution Updated: " + std::to_string(cfg.width) + "x" + std::to_string(cfg.height));
            std::lock_guard<std::mutex> lk(g_cfgMutex); 
            g_videoConfigs[pS] = cfg;
            g_lastKnownConfig = cfg;
            for (auto& p : g_videoConfigs) { if (p.second.width == 0) p.second = cfg; }
        }
    }
    return hr;
}

/*
 * AfterGraphPinsConnected — run once pins are successfully linked (Connect or ConnectDirect).
 *
 * Why this exists:
 *   DirectShow does not give a single "frame callback"; we watermark by vtable-hooking the
 *   input pin's IMemInputPin::Receive. That only works if we patch the *instance* that actually
 *   carries video after the graph is built.
 *
 * What we do:
 *   1) Output pin: if it exposes IAMStreamConfig, hook SetFormat so resolution / subtype changes
 *      (e.g. NV12 vs MJPEG) refresh g_videoConfigs.
 *   2) Input pin: read the negotiated AM_MEDIA_TYPE, stash VideoConfig under IMemInputPin*,
 *      and hook Receive so each buffer hits HookedReceive → ProcessWatermarkInternal / MJPEG path.
 *
 * Kept in one function so Connect and ConnectDirect stay identical (QWC-style parity).
 */
static void AfterGraphPinsConnected(IGraphBuilder* pGraph, IPin* pOut, IPin* pIn) {
    if (!pGraph || !pOut || !pIn) return;
    IAMStreamConfig* pCfg = nullptr;
    if (SUCCEEDED(pOut->QueryInterface(IID_IAMStreamConfig, (void**)&pCfg))) {
        PatchVTable(pCfg, kSlot_IAMStreamConfig_SetFormat, (void*)&HookedDSSetFormat, g_origDSSetFormatMap);
        pCfg->Release();
    }
    AM_MEDIA_TYPE mt = {};
    if (SUCCEEDED(pIn->ConnectionMediaType(&mt))) {
        const BITMAPINFOHEADER* bih = nullptr;
        if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER))
            bih = &((VIDEOINFOHEADER*)mt.pbFormat)->bmiHeader;
        else if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2))
            bih = &((VIDEOINFOHEADER2*)mt.pbFormat)->bmiHeader;

        if (bih) {
            VideoConfig cfg;
            cfg.width = bih->biWidth;
            cfg.height = (int)abs(bih->biHeight);
            cfg.isNV12 = (mt.subtype == MEDIASUBTYPE_NV12);
            cfg.isI420 = IsPlanar420Subtype(mt.subtype);
            cfg.isCompressed = (mt.subtype == MEDIASUBTYPE_MJPG);
            DebugLog::log("[WebcamDLL] DS connected media: " + std::to_string(cfg.width) + "x" + std::to_string(cfg.height) +
                (cfg.isCompressed ? " MJPG" : (cfg.isI420 ? " I420" : (cfg.isNV12 ? " NV12" : " YUY2/other"))));
            IMemInputPin* pMip = nullptr;
            if (SUCCEEDED(pIn->QueryInterface(IID_IMemInputPin, (void**)&pMip))) {
                { std::lock_guard<std::mutex> lk(g_cfgMutex); g_videoConfigs[pMip] = cfg; }
                PatchVTable(pMip, kSlot_IMemInputPin_Receive, (void*)&HookedReceive, g_origReceiveMap);
                DebugLog::log("[WebcamDLL] DS patched IMemInputPin::Receive.");
                pMip->Release();
            }
        }
        FreeMediaType(mt);
    }
}

HRESULT STDMETHODCALLTYPE HookedGraphConnect(IGraphBuilder* pS, IPin* pO, IPin* pI) {
    void** vt = *(void***)pS;
    PGraphConnect orig = nullptr;
    { std::lock_guard<std::mutex> lk(g_hookMutex); auto it = g_origGraphConnectMap.find(vt); if (it != g_origGraphConnectMap.end()) orig = it->second; }
    HRESULT hr = orig ? orig(pS, pO, pI) : E_FAIL;
    if (SUCCEEDED(hr))
        AfterGraphPinsConnected(pS, pO, pI);
    return hr;
}

/*
 * HookedConnectDirect — IFilterGraph::ConnectDirect trampoline.
 *
 * Browsers and some capture stacks connect filters with ConnectDirect instead of IGraphBuilder::Connect.
 * If we only hook Connect, those graphs never reach AfterGraphPinsConnected, so IMemInputPin::Receive
 * stays unhooked and no watermark runs. Same vtable layout as QWC (IFILTERGRAPH_CONNECTDIRECT_SLOT = 7).
 *
 * pThis is IFilterGraph*; we QI to IGraphBuilder* only to reuse AfterGraphPinsConnected's signature.
 */
HRESULT STDMETHODCALLTYPE HookedConnectDirect(IFilterGraph* pThis, IPin* pO, IPin* pI) {
    void** vt = *(void***)pThis;
    PConnectDirect orig = nullptr;
    { std::lock_guard<std::mutex> lk(g_hookMutex); auto it = g_origConnectDirectMap.find(vt); if (it != g_origConnectDirectMap.end()) orig = it->second; }
    HRESULT hr = orig ? orig(pThis, pO, pI) : E_FAIL;
    if (SUCCEEDED(hr)) {
        IGraphBuilder* pGraph = nullptr;
        if (SUCCEEDED(pThis->QueryInterface(IID_IGraphBuilder, (void**)&pGraph))) {
            AfterGraphPinsConnected(pGraph, pO, pI);
            pGraph->Release();
        }
    }
    return hr;
}

typedef HRESULT(WINAPI* PCoCreate)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static PCoCreate g_origCoCreate = NULL;

/*
 * PatchFilterGraphInstance — patch the filter graph's primary vtable (Connect + ConnectDirect).
 *
 * CLSID_FilterGraph exposes IUnknown / IFilterGraph / IGraphBuilder on one object with one vtable;
 * *ppv from CoCreateInstance is the object pointer even when riid is IID_IUnknown, so slot indices
 * (e.g. Connect at 11, ConnectDirect at 7) still refer to the same table as in QWC qwcd/dllmain.cpp.
 */
static void PatchFilterGraphInstance(void* pGraphObj) {
    if (!pGraphObj) return;
    PatchVTable(pGraphObj, kSlot_IGraphBuilder_Connect, (void*)&HookedGraphConnect, g_origGraphConnectMap);
    PatchVTable(pGraphObj, kSlot_IFilterGraph_ConnectDirect, (void*)&HookedConnectDirect, g_origConnectDirectMap);
}

/*
 * HookedCoCreate — MinHook on ole32!CoCreateInstance.
 *
 * We only patch when clsid is CLSID_FilterGraph (same rule as QWC). Other coclasses can implement
 * IGraphBuilder; patching by riid alone would risk corrupting an unrelated vtable. Webcam pipelines
 * use the standard Filter Graph object for DirectShow capture.
 */
HRESULT WINAPI HookedCoCreate(REFCLSID clsid, LPUNKNOWN pU, DWORD ctx, REFIID riid, LPVOID* ppv) {
    HRESULT hr = g_origCoCreate(clsid, pU, ctx, riid, ppv);
    if (FAILED(hr) || !ppv || !*ppv) return hr;

    if (IsEqualCLSID(clsid, CLSID_FilterGraph))
        PatchFilterGraphInstance(*ppv);

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
