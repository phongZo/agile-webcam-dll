#include "pch.h"
#include <windows.h>
#include <string>
#include <atomic>
#include <mutex>
#include <dshow.h>
#include <tlhelp32.h>
#include <psapi.h>
#include "DebugLog.h"
#include "../packages/minhook.1.3.3/lib/native/include/MinHook.h"

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")

#ifdef _WIN64
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x64-v141-mt.lib")
#else
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x86-v141-mt.lib")
#endif

// --- Global State ---
static HINSTANCE g_hModule = NULL;
static std::atomic<bool> g_bUnloading(false);
static std::atomic<bool> g_bInitialized(false);
static std::atomic<bool> g_bReceiveHooked(false);
static std::mutex g_hookMutex;
static std::mutex g_drawMutex;

// --- Typedefs ---
typedef HRESULT(WINAPI* PCoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static PCoCreateInstance g_origCoCreateInstance = NULL;

typedef HRESULT(STDMETHODCALLTYPE* PGraphConnect)(IGraphBuilder*, IPin*, IPin*);
static PGraphConnect g_origGraphConnect = NULL;

typedef HRESULT(STDMETHODCALLTYPE* PReceive)(IMemInputPin*, IMediaSample*);
static PReceive g_origReceive = NULL;

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

// --- YUY2 Drawing Engine ---
void DrawAgileMarkStyle(BYTE* pData, int width, int height, int base_x, int base_y, BYTE Y, BYTE U, BYTE V) {
    int stride = width * 2;
    // Vẽ chữ AGILEMARK nghiêng nhẹ bằng cách tịnh tiến y theo x
    for (int i = 0; i < 9; i++) {
        int char_x = base_x + i * 14;
        int char_y = base_y + i * 4; // Tạo độ nghiêng cho dòng chữ

        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                if (g_agilemark_font[i][r] & (0x80 >> c)) {
                    // Phóng to pixel 2x2
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

void ProcessWatermarkMaster(BYTE* pData, long size, int width, int height) {
    if (!pData || size < (width * height * 2)) return;
    std::lock_guard<std::mutex> lock(g_drawMutex);

    BYTE Y = 76, U = 84, V = 255; // Red Color

    // Vẽ lưới AGILEMARK chuẩn Master
    int stepX = 250;
    int stepY = 180;

    for (int y = -100; y < height; y += stepY) {
        for (int x = -100; x < width; x += stepX) {
            DrawAgileMarkStyle(pData, width, height, x, y, Y, U, V);
        }
    }
}

// --- Hook Implementations ---
HRESULT STDMETHODCALLTYPE HookedReceive(IMemInputPin* pSelf, IMediaSample* pSample) {
    if (pSample && !g_bUnloading.load()) {
        BYTE* pBuffer = NULL;
        if (SUCCEEDED(pSample->GetPointer(&pBuffer))) {
            long actualLen = pSample->GetActualDataLength();
            int w = 640, h = 480;
            if (actualLen >= 1280 * 720 * 2) { w = 1280; h = 720; }
            ProcessWatermarkMaster(pBuffer, actualLen, w, h);
        }
    }
    return g_origReceive(pSelf, pSample);
}

void HookMemInputSafe(IMemInputPin* pMemInput) {
    if (!pMemInput) return;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (g_bReceiveHooked.load()) return;

    void** vtable = *(void***)pMemInput;
    if (MH_CreateHook(vtable[6], &HookedReceive, (LPVOID*)&g_origReceive) == MH_OK) {
        MH_EnableHook(vtable[6]);
        g_bReceiveHooked = true;
        DebugLog::log("[WebcamDLL] Master AGILEMARK Style Active");
    }
}

HRESULT STDMETHODCALLTYPE HookedGraphConnect(IGraphBuilder* pSelf, IPin* pOut, IPin* pIn) {
    HRESULT hr = g_origGraphConnect(pSelf, pOut, pIn);
    if (SUCCEEDED(hr)) {
        IMemInputPin* pMemInput = NULL;
        if (SUCCEEDED(pIn->QueryInterface(IID_IMemInputPin, (void**)&pMemInput))) {
            HookMemInputSafe(pMemInput);
            pMemInput->Release();
        }
    }
    return hr;
}

HRESULT WINAPI HookedCoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv) {
    HRESULT hr = g_origCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (riid == IID_IGraphBuilder || riid == IID_IFilterGraph) {
            void** vtable = *(void***)*ppv;
            std::lock_guard<std::mutex> lock(g_hookMutex);
            if (MH_CreateHook(vtable[11], &HookedGraphConnect, (LPVOID*)&g_origGraphConnect) == MH_OK) {
                MH_EnableHook(vtable[11]);
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
            do { if (_wcsicmp(pe.szExeFile, L"WebcamWatermark.exe") == 0) { found = true; break; } } while (Process32NextW(h, &pe));
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
    DebugLog::log("[WebcamDLL] StartWatch v17.0.0 (Master AgileMark Style)");

    if (MH_Initialize() == MH_OK) {
        HMODULE hOle32 = GetModuleHandleW(L"ole32.dll");
        if (hOle32) {
            void* p = (void*)GetProcAddress(hOle32, "CoCreateInstance");
            MH_CreateHook(p, &HookedCoCreateInstance, (LPVOID*)&g_origCoCreateInstance);
            MH_EnableHook(p);
        }
    }
    CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hMod); g_hModule = hMod; }
    return TRUE;
}
