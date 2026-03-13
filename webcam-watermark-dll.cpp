#include "pch.h"
#include <windows.h>
#include <string>
#include <map>
#include <mutex>
#include <vector>
#include <atomic>
#include <dshow.h>
#include <dvdmedia.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <tlhelp32.h>
#include <psapi.h>
#include "DebugLog.h"
#include "../packages/minhook.1.3.3/lib/native/include/MinHook.h"

// --- LINKER LIBRARIES ---
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
static std::atomic<int> g_activeCalls(0); 
static std::atomic<bool> g_bInitialized(false);

// --- MF Interface Typedefs ---
typedef HRESULT(WINAPI* PMFCreateSourceReaderFromMediaSource)(IMFMediaSource*, IMFAttributes*, IMFSourceReader**);
static PMFCreateSourceReaderFromMediaSource g_origMFCreateSourceReaderMS = NULL;

typedef HRESULT(WINAPI* PMFCreateSourceReaderFromUnknown)(IUnknown*, IMFAttributes*, IMFSourceReader**);
static PMFCreateSourceReaderFromUnknown g_origMFCreateSourceReaderUnk = NULL;

typedef HRESULT(WINAPI* PMFCreateSourceReaderFromByteStream)(IMFByteStream*, IMFAttributes*, IMFSourceReader**);
static PMFCreateSourceReaderFromByteStream g_origMFCreateSourceReaderBS = NULL;

typedef HRESULT(WINAPI* PMFCreateDeviceSource)(IMFAttributes*, IMFMediaSource**);
static PMFCreateDeviceSource g_origMFCreateDeviceSource = NULL;

typedef HRESULT(STDMETHODCALLTYPE* POnReadSample)(IMFSourceReaderCallback*, HRESULT, DWORD, DWORD, LONGLONG, IMFSample*);
static POnReadSample g_origOnReadSample = NULL;

// --- Helper: Draw Grid on Frame ---
void DrawGridPattern(BYTE* pData, int width, int height, long stride) {
    for (int y = 0; y < height; y += 50) {
        memset(pData + (y * stride), 255, (width < stride) ? width : stride);
    }
    for (int x = 0; x < width; x += 50) {
        for (int y = 0; y < height; y++) {
            pData[y * stride + x] = 255;
        }
    }
}

void ProcessMFSample(IMFSample* pSample) {
    if (!pSample || g_bUnloading.load()) return;
    g_activeCalls++;
    IMFMediaBuffer* pBuffer = NULL;
    if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer))) {
        BYTE* pData = NULL; LONG lStride = 0; DWORD cbCurrent = 0;
        IMF2DBuffer* p2DBuffer = NULL;
        if (SUCCEEDED(pBuffer->QueryInterface(IID_IMF2DBuffer, (void**)&p2DBuffer))) {
            if (SUCCEEDED(p2DBuffer->Lock2D(&pData, &lStride))) {
                DrawGridPattern(pData, (int)abs(lStride), 480, lStride);
                p2DBuffer->Unlock2D();
            }
            p2DBuffer->Release();
        } else if (SUCCEEDED(pBuffer->Lock(&pData, NULL, &cbCurrent))) {
            DrawGridPattern(pData, 640, 480, 640);
            pBuffer->Unlock();
        }
        pBuffer->Release();
    }
    g_activeCalls--;
}

// --- Hooks Implementation ---
HRESULT STDMETHODCALLTYPE HookedOnReadSample(IMFSourceReaderCallback* pSelf, HRESULT hrStatus, DWORD dwStreamIndex, DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample* pSample) {
    if (SUCCEEDED(hrStatus) && pSample) ProcessMFSample(pSample);
    return g_origOnReadSample(pSelf, hrStatus, dwStreamIndex, dwStreamFlags, llTimestamp, pSample);
}

void HookSourceReader(IMFSourceReader* pReader, IMFAttributes* pAttributes) {
    if (!pReader) return;
    if (pAttributes) {
        IUnknown* pUnkCallback = NULL;
        if (SUCCEEDED(pAttributes->GetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, IID_IUnknown, (LPVOID*)&pUnkCallback))) {
            void** vtable = *(void***)pUnkCallback;
            if (MH_CreateHook(vtable[3], &HookedOnReadSample, (LPVOID*)&g_origOnReadSample) == MH_OK) {
                MH_EnableHook(vtable[3]);
                DebugLog::log("[WebcamDLL] Hooked Async Callback");
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

HRESULT WINAPI HookedMFCreateDeviceSource(IMFAttributes* pAttr, IMFMediaSource** ppMS) {
    HRESULT hr = g_origMFCreateDeviceSource(pAttr, ppMS);
    if (SUCCEEDED(hr)) DebugLog::log("[WebcamDLL] Device Source created");
    return hr;
}

// --- Watchdog & Unload ---
static bool IsAgileMarkPresent() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W pe{ sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do { 
            if (_wcsicmp(pe.szExeFile, L"AgileMark.exe") == 0 || _wcsicmp(pe.szExeFile, L"WebcamWatermark.exe") == 0) {
                found = true; break; 
            } 
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

void InitiateHardUnload() {
    if (g_bUnloading.exchange(true)) return;
    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(1000);
    MH_Uninitialize();
    DebugLog::log("[WebcamDLL] Safe unload complete.");
    FreeLibraryAndExitThread(g_hModule, 0);
}

DWORD WINAPI WatchdogThread(LPVOID) {
    while (!g_bUnloading.load()) {
        Sleep(3000);
        if (!IsAgileMarkPresent()) { InitiateHardUnload(); return 0; }
    }
    return 0;
}

void LogLoadedModules() {
    HMODULE hMods[1024]; DWORD cbNeeded;
    if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded)) {
        for (int i = 0; i < (cbNeeded / sizeof(HMODULE)); i++) {
            TCHAR szModName[MAX_PATH];
            if (GetModuleBaseName(GetCurrentProcess(), hMods[i], szModName, sizeof(szModName) / sizeof(TCHAR))) {
                std::wstring ws(szModName);
                std::string s(ws.begin(), ws.end());
                if (s.find("mf") != std::string::npos || s.find("d3d") != std::string::npos)
                    DebugLog::log("[WebcamDLL] Module: " + s);
            }
        }
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI StartWatch(LPVOID lp) {
    if (g_bInitialized.exchange(true)) return 0;
    DebugLog::initialize();
    DebugLog::log("[WebcamDLL] StartWatch v2.7.1");
    LogLoadedModules();

    if (MH_Initialize() == MH_OK) {
        HMODULE hMF = GetModuleHandleW(L"Mfreadwrite.dll");
        if (!hMF) hMF = LoadLibraryW(L"Mfreadwrite.dll");
        HMODULE hMFPlat = GetModuleHandleW(L"Mfplat.dll");
        if (!hMFPlat) hMFPlat = LoadLibraryW(L"Mfplat.dll");
        
        if (hMF) {
            void* p1 = (void*)GetProcAddress(hMF, "MFCreateSourceReaderFromMediaSource");
            void* p2 = (void*)GetProcAddress(hMF, "MFCreateSourceReaderFromUnknown");
            void* p3 = (void*)GetProcAddress(hMF, "MFCreateSourceReaderFromByteStream");
            if (p1) MH_CreateHook(p1, &HookedMFCreateSourceReaderFromMediaSource, (LPVOID*)&g_origMFCreateSourceReaderMS);
            if (p2) MH_CreateHook(p2, &HookedMFCreateSourceReaderFromUnknown, (LPVOID*)&g_origMFCreateSourceReaderUnk);
            if (p3) MH_CreateHook(p3, &HookedMFCreateSourceReaderFromByteStream, (LPVOID*)&g_origMFCreateSourceReaderBS);
        }
        if (hMFPlat) {
            void* p4 = (void*)GetProcAddress(hMFPlat, "MFCreateDeviceSource");
            if (p4) MH_CreateHook(p4, &HookedMFCreateDeviceSource, (LPVOID*)&g_origMFCreateDeviceSource);
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
