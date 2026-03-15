#include "pch.h"
#include <windows.h>
#include <string>
#include <atomic>
#include <mutex>
#include <dshow.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <tlhelp32.h>
#include <psapi.h>
#include "DebugLog.h"
#include "../packages/minhook.1.3.3/lib/native/include/MinHook.h"

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

#ifdef _WIN64
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x64-v141-mt.lib")
#else
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x86-v141-mt.lib")
#endif

// --- Global State ---
static HINSTANCE g_hModule = NULL;
static std::atomic<bool> g_bUnloading(false);
static std::atomic<bool> g_bInitialized(false);
static std::mutex g_hookMutex;
static std::mutex g_drawMutex;

// Flags to prevent multiple hooks causing crashes in Teams/Webex
static std::atomic<bool> g_bDShowHooked(false);
static std::atomic<bool> g_bMFHooked(false);
static std::atomic<bool> g_bMFCallbackHooked(false);

// --- AGILEMARK Standard Bitmap Font 8x8 ---
static unsigned char g_agilemark_font[9][8] = {
    {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00}, // A
    {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00}, // G
    {0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00}, // I
    {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00}, // L
    {0x7E, 0x60, 0x60, 0x78, 0x60, 0x60, 0x7E, 0x00}, // E
    {0x66, 0x7E, 0x7E, 0x66, 0x66, 0x66, 0x66, 0x00}, // M
    {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00}, // A
    {0x7C, 0x66, 0x66, 0x7C, 0x78, 0x66, 0x66, 0x00}, // R
    {0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00}  // K
};

// --- Drawing Engines ---
void DrawAgileMarkYUY2(BYTE* pData, int width, int height, int base_x, int base_y, BYTE Y, BYTE U, BYTE V) {
    int stride = width * 2;
    for (int i = 0; i < 9; i++) {
        int char_x = base_x + i * 14;
        int char_y = base_y + i * 4;
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                if (g_agilemark_font[i][r] & (0x80 >> c)) {
                    for (int dy = 0; dy < 2; dy++) {
                        for (int dx = 0; dx < 2; dx++) {
                            int px = char_x + c * 2 + dx;
                            int py = char_y + r * 2 + dy;
                            if (px >= 0 && px < width && py >= 0 && py < height) {
                                int pos = py * stride + px * 2;
                                pData[pos] = Y;
                                int uv_pos = py * stride + (px & ~1) * 2 + 1;
                                if (uv_pos + 2 < width * height * 2) {
                                    pData[uv_pos] = U; pData[uv_pos + 2] = V;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void DrawAgileMarkNV12(BYTE* pY, BYTE* pUV, int width, int height, int stride, int base_x, int base_y, BYTE Y, BYTE U, BYTE V) {
    for (int i = 0; i < 9; i++) {
        int char_x = base_x + i * 14;
        int char_y = base_y + i * 4;
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                if (g_agilemark_font[i][r] & (0x80 >> c)) {
                    for (int dy = 0; dy < 2; dy++) {
                        for (int dx = 0; dx < 2; dx++) {
                            int px = char_x + c * 2 + dx;
                            int py = char_y + r * 2 + dy;
                            if (px >= 0 && px < width && py >= 0 && py < height) {
                                pY[py * stride + px] = Y;
                                int uv_x = px & ~1;
                                int uv_y = py / 2;
                                int uv_pos = uv_y * stride + uv_x;
                                pUV[uv_pos] = U;
                                pUV[uv_pos + 1] = V;
                            }
                        }
                    }
                }
            }
        }
    }
}

void ProcessWatermark(BYTE* pData, int width, int height, bool isNV12, int stride = 0) {
    std::lock_guard<std::mutex> lock(g_drawMutex);
    BYTE Y = 76, U = 84, V = 255; // Red Color
    int stepX = 250, stepY = 180;
    if (stride == 0) stride = width;

    for (int y = -100; y < height; y += stepY) {
        for (int x = -100; x < width; x += stepX) {
            if (isNV12) {
                BYTE* pUV = pData + stride * height;
                DrawAgileMarkNV12(pData, pUV, width, height, stride, x, y, Y, U, V);
            } else {
                DrawAgileMarkYUY2(pData, width, height, x, y, Y, U, V);
            }
        }
    }
}

// --- Media Foundation Hooks ---
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

void ProcessMFSample(IMFSample* pSample) {
    if (!pSample || g_bUnloading.load()) return;
    IMFMediaBuffer* pBuffer = NULL;
    if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer))) {
        BYTE* pData = NULL; LONG lStride = 0; DWORD cbCurrent = 0;
        IMF2DBuffer* p2DBuffer = NULL;
        if (SUCCEEDED(pBuffer->QueryInterface(IID_IMF2DBuffer, (void**)&p2DBuffer))) {
            if (SUCCEEDED(p2DBuffer->Lock2D(&pData, &lStride))) {
                // Determine height from stride/buffer size if possible, otherwise assume 480
                int h = 480;
                ProcessWatermark(pData, (int)abs(lStride), h, true, (int)abs(lStride));
                p2DBuffer->Unlock2D();
            }
            p2DBuffer->Release();
        } else if (SUCCEEDED(pBuffer->Lock(&pData, NULL, &cbCurrent))) {
            ProcessWatermark(pData, 640, 480, false);
            pBuffer->Unlock();
        }
        pBuffer->Release();
    }
}

HRESULT STDMETHODCALLTYPE HookedOnReadSample(IMFSourceReaderCallback* pSelf, HRESULT hrStatus, DWORD dwStreamIndex, DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample* pSample) {
    if (SUCCEEDED(hrStatus) && pSample) ProcessMFSample(pSample);
    return g_origOnReadSample(pSelf, hrStatus, dwStreamIndex, dwStreamFlags, llTimestamp, pSample);
}

HRESULT STDMETHODCALLTYPE HookedReadSample(IMFSourceReader* pSelf, DWORD dwStreamIndex, DWORD dwControlFlags, DWORD* pdwActualStreamIndex, DWORD* pdwStreamFlags, LONGLONG* pllTimestamp, IMFSample** ppSample) {
    HRESULT hr = g_origReadSample(pSelf, dwStreamIndex, dwControlFlags, pdwActualStreamIndex, pdwStreamFlags, pllTimestamp, ppSample);
    if (SUCCEEDED(hr) && ppSample && *ppSample) ProcessMFSample(*ppSample);
    return hr;
}

void HookSourceReader(IMFSourceReader* pReader, IMFAttributes* pAttributes) {
    if (!pReader) return;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    
    if (!g_bMFHooked.load()) {
        void** vtable = *(void***)pReader;
        if (MH_CreateHook(vtable[9], &HookedReadSample, (LPVOID*)&g_origReadSample) == MH_OK) {
            MH_EnableHook(vtable[9]);
            g_bMFHooked = true;
            DebugLog::log("[WebcamDLL] Hooked IMFSourceReader::ReadSample");
        }
    }

    if (pAttributes && !g_bMFCallbackHooked.load()) {
        IUnknown* pUnkCallback = NULL;
        if (SUCCEEDED(pAttributes->GetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, IID_IUnknown, (LPVOID*)&pUnkCallback))) {
            void** cbVtable = *(void***)pUnkCallback;
            if (MH_CreateHook(cbVtable[3], &HookedOnReadSample, (LPVOID*)&g_origOnReadSample) == MH_OK) {
                MH_EnableHook(cbVtable[3]);
                g_bMFCallbackHooked = true;
                DebugLog::log("[WebcamDLL] Hooked IMFSourceReaderCallback::OnReadSample");
            }
            pUnkCallback->Release();
        }
    }
}

HRESULT WINAPI HookedMFCreateSourceReaderFromMediaSource(IMFMediaSource* pMS, IMFAttributes* pAttr, IMFSourceReader** ppSR) {
    HRESULT hr = g_origMFCreateSourceReaderMS(pMS, pAttr, ppSR);
    if (SUCCEEDED(hr) && ppSR && *ppSR) HookSourceReader(*ppSR, pAttr);
    return hr;
}

HRESULT WINAPI HookedMFCreateSourceReaderFromUnknown(IUnknown* pUnk, IMFAttributes* pAttr, IMFSourceReader** ppSR) {
    HRESULT hr = g_origMFCreateSourceReaderUnk(pUnk, pAttr, ppSR);
    if (SUCCEEDED(hr) && ppSR && *ppSR) HookSourceReader(*ppSR, pAttr);
    return hr;
}

HRESULT WINAPI HookedMFCreateSourceReaderFromByteStream(IMFByteStream* pBS, IMFAttributes* pAttr, IMFSourceReader** ppSR) {
    HRESULT hr = g_origMFCreateSourceReaderBS(pBS, pAttr, ppSR);
    if (SUCCEEDED(hr) && ppSR && *ppSR) HookSourceReader(*ppSR, pAttr);
    return hr;
}

// --- DirectShow Hooks ---
typedef HRESULT(WINAPI* PCoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static PCoCreateInstance g_origCoCreateInstance = NULL;

typedef HRESULT(STDMETHODCALLTYPE* PGraphConnect)(IGraphBuilder*, IPin*, IPin*);
static PGraphConnect g_origGraphConnect = NULL;

typedef HRESULT(STDMETHODCALLTYPE* PReceive)(IMemInputPin*, IMediaSample*);
static PReceive g_origReceive = NULL;

HRESULT STDMETHODCALLTYPE HookedReceive(IMemInputPin* pSelf, IMediaSample* pSample) {
    if (pSample && !g_bUnloading.load()) {
        BYTE* pBuffer = NULL;
        if (SUCCEEDED(pSample->GetPointer(&pBuffer))) {
            long actualLen = pSample->GetActualDataLength();
            int w = 640, h = 480;
            if (actualLen >= 1280 * 720 * 2) { w = 1280; h = 720; }
            ProcessWatermark(pBuffer, w, h, false);
        }
    }
    return g_origReceive(pSelf, pSample);
}

HRESULT STDMETHODCALLTYPE HookedGraphConnect(IGraphBuilder* pSelf, IPin* pOut, IPin* pIn) {
    HRESULT hr = g_origGraphConnect(pSelf, pOut, pIn);
    if (SUCCEEDED(hr)) {
        IMemInputPin* pMemInput = NULL;
        if (SUCCEEDED(pIn->QueryInterface(IID_IMemInputPin, (void**)&pMemInput))) {
            std::lock_guard<std::mutex> lock(g_hookMutex);
            if (!g_bDShowHooked.load()) {
                void** vtable = *(void***)pMemInput;
                if (MH_CreateHook(vtable[6], &HookedReceive, (LPVOID*)&g_origReceive) == MH_OK) {
                    MH_EnableHook(vtable[6]);
                    g_bDShowHooked = true;
                    DebugLog::log("[WebcamDLL] Hooked IMemInputPin::Receive");
                }
            }
            pMemInput->Release();
        }
    }
    return hr;
}

HRESULT WINAPI HookedCoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv) {
    HRESULT hr = g_origCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (riid == IID_IGraphBuilder || riid == IID_IFilterGraph) {
            std::lock_guard<std::mutex> lock(g_hookMutex);
            void** vtable = *(void***)*ppv;
            // We can hook multiple graph builders, but they usually share the same vtable
            if (MH_CreateHook(vtable[11], &HookedGraphConnect, (LPVOID*)&g_origGraphConnect) == MH_OK) {
                MH_EnableHook(vtable[11]);
                DebugLog::log("[WebcamDLL] Hooked IGraphBuilder::Connect");
            }
        }
    }
    return hr;
}

// --- Watchdog ---
DWORD WINAPI WatchdogThread(LPVOID) {
    while (!g_bUnloading.load()) {
        Sleep(3000);
        HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{sizeof(pe)};
        bool found = false;
        if (Process32FirstW(h, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"WebcamWatermark.exe") == 0) { found = true; break; }
            } while (Process32NextW(h, &pe));
        }
        CloseHandle(h);
        if (!found) {
            g_bUnloading = true;
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            FreeLibraryAndExitThread(g_hModule, 0);
        }
    }
    return 0;
}

extern "C" __declspec(dllexport) DWORD WINAPI StartWatch(LPVOID lp) {
    if (g_bInitialized.exchange(true)) return 0;
    DebugLog::initialize();
    DebugLog::log("[WebcamDLL] StartWatch v18.1.0 (Safe Unified)");

    if (MH_Initialize() == MH_OK) {
        // DirectShow Hook
        HMODULE hOle32 = GetModuleHandleW(L"ole32.dll");
        if (hOle32) {
            void* p = (void*)GetProcAddress(hOle32, "CoCreateInstance");
            MH_CreateHook(p, &HookedCoCreateInstance, (LPVOID*)&g_origCoCreateInstance);
        }

        // Media Foundation Hooks
        HMODULE hMF = GetModuleHandleW(L"Mfreadwrite.dll");
        if (!hMF) hMF = LoadLibraryW(L"Mfreadwrite.dll");
        if (hMF) {
            void* p1 = (void*)GetProcAddress(hMF, "MFCreateSourceReaderFromMediaSource");
            void* p2 = (void*)GetProcAddress(hMF, "MFCreateSourceReaderFromUnknown");
            void* p3 = (void*)GetProcAddress(hMF, "MFCreateSourceReaderFromByteStream");
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
