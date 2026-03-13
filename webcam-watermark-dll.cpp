#include "pch.h"
#include <windows.h>
#include <string>
#include <map>
#include <mutex>
#include <vector>
#include <atomic>
#include <dshow.h>
#include <dvdmedia.h>
#include <Lmcons.h>
#include <tlhelp32.h>
#include "DebugLog.h"
#include "../packages/minhook.1.3.3/lib/native/include/MinHook.h"
#include "../packages/nlohmann.json.3.12.0/build/native/include/nlohmann/json.hpp"

// Renderer logic headers (Copied from Window DLL)
#include "Renderer/RenderSnapshot.h"
#include "Renderer/MarkerJson.h"

using json = nlohmann::json;

// --- LINKER LIBRARIES ---
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "secur32.lib")

#ifdef _WIN64
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x64-v141-mt.lib")
#else
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x86-v141-mt.lib")
#endif

// --- Global State ---
static HINSTANCE g_hModule = NULL;
static ULONG_PTR g_gdiplusToken = 0;
static const wchar_t* kPipeName = L"\\\\.\\pipe\\dll_qaKOab5VPyK4ar4A6sfm2VZ0";
static RenderSnapshot g_Snapshot;
static std::mutex g_SnapMutex;

// Safe Unload Mechanism
static std::atomic<bool> g_bUnloading(false);
static std::atomic<int> g_activeCalls(0); 

// --- COM Interface Typedefs ---
typedef HRESULT(WINAPI* PCoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
PCoCreateInstance g_origCoCreateInstance = NULL;

typedef HRESULT(STDMETHODCALLTYPE* PAddFilter)(IFilterGraph*, IBaseFilter*, LPCWSTR);
PAddFilter g_origAddFilter = NULL;

typedef HRESULT(STDMETHODCALLTYPE* PReceive)(IMemInputPin*, IMediaSample*);
PReceive g_origReceive = NULL;

// --- Adaptive Format Storage (Restored from stable V25.1) ---
struct StreamFormat {
    int w = 0;
    int h = 0;
    int bpp = 0;
    bool isValid = false;
};
std::map<IMemInputPin*, StreamFormat> g_PinCache;
std::mutex g_CacheMutex;

// --- Helper Functions: Video Info Decoding ---
bool GetVideoInfo(AM_MEDIA_TYPE* pmt, int& w, int& h, int& bpp) {
    if (!pmt) return false;
    if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
        w = vih->bmiHeader.biWidth; h = abs(vih->bmiHeader.biHeight); bpp = vih->bmiHeader.biBitCount;
        return true;
    }
    if (pmt->formattype == FORMAT_VideoInfo2 && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        VIDEOINFOHEADER2* vih2 = (VIDEOINFOHEADER2*)pmt->pbFormat;
        w = vih2->bmiHeader.biWidth; h = abs(vih2->bmiHeader.biHeight); bpp = vih2->bmiHeader.biBitCount;
        return true;
    }
    return false;
}

std::wstring Utf8ToUtf16(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring ws(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &ws[0], len);
    return ws;
}

Gdiplus::Color ParseHexColor(const std::wstring& hex, float opacityMul) {
    std::wstring s = hex; if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6) return Gdiplus::Color((BYTE)(255 * opacityMul), 255, 255, 255);
    auto h2i = [](wchar_t c) {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return 0;
    };
    int r = h2i(s[0]) * 16 + h2i(s[1]);
    int g = h2i(s[2]) * 16 + h2i(s[3]);
    int b = h2i(s[4]) * 16 + h2i(s[5]);
    return Gdiplus::Color((BYTE)(255 * opacityMul), (BYTE)r, (BYTE)g, (BYTE)b);
}

// --- Macro Expansion: System Variables ---
std::wstring ExpandMacros(const std::wstring& fmt) {
    std::wstring out = fmt;
    auto replaceAll = [&](const std::wstring& needle, const std::wstring& repl) {
        size_t pos = 0;
        while ((pos = out.find(needle, pos)) != std::wstring::npos) {
            out.replace(pos, needle.length(), repl);
            pos += repl.length();
        }
    };

    // User Information
    wchar_t user[UNLEN + 1] = { 0 }; DWORD ul = UNLEN + 1;
    if (GetUserNameW(user, &ul)) {
        replaceAll(L"{username}", user);
        replaceAll(L"{Username}", user);
        replaceAll(L"{UserName}", user);
    }

    // Machine Information
    wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1] = { 0 }; DWORD cl = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(comp, &cl)) {
        replaceAll(L"{machinename}", comp);
        replaceAll(L"{MachineName}", comp);
    }

    // Date and Time
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t bufD[64], bufT[64], bufLT[64];
    swprintf_s(bufD, L"%02d/%02d/%04d", st.wDay, st.wMonth, st.wYear);
    swprintf_s(bufT, L"%02d:%02d", st.wHour, st.wMinute);
    swprintf_s(bufLT, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

    replaceAll(L"{shortdate}", bufD);   replaceAll(L"{ShortDate}", bufD);
    replaceAll(L"{shorttime}", bufT);   replaceAll(L"{ShortTime}", bufT);
    replaceAll(L"{longtime}", bufLT);   replaceAll(L"{LongTime}", bufLT);

    return out;
}

static bool IsAgileMarkPresent() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W pe{ sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do { if (_wcsicmp(pe.szExeFile, L"AgileMark.exe") == 0) { found = true; break; } } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Gentle unload to prevent application crash
void InitiateHardUnload() {
    if (g_bUnloading.exchange(true)) return;
    
    // Ghost Mode: Stop all rendering immediately
    Sleep(500); 

    // Wait for active streaming calls to finish
    int retries = 50;
    while (g_activeCalls.load() > 0 && retries-- > 0) Sleep(50);

    // Disable Hooks
    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(1000); // Buffer time for trampoline jumps

    // Release GDI+ and MinHook
    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
    MH_Uninitialize();
    
    DebugLog::log("[WebcamDLL] Safe unload complete.");
    FreeLibraryAndExitThread(g_hModule, 0);
}

// --- IPC Thread: Receive configuration from service ---
DWORD WINAPI IpcThread(LPVOID) {
    std::string accumulator;
    while (!g_bUnloading.load()) {
        HANDLE hPipe = CreateFileW(kPipeName, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hPipe == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }
        
        char buf[4096]; DWORD bytesRead = 0;
        while (!g_bUnloading.load() && ReadFile(hPipe, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
            accumulator.append(buf, bytesRead);
            size_t pos;
            while ((pos = accumulator.find('\n')) != std::string::npos) {
                std::string line = accumulator.substr(0, pos);
                try {
                    auto j = json::parse(line);
                    if (j.value("cmd", "") == "CMD_RENDER_UPDATE" && j.contains("store")) {
                        const auto& m = j["store"]["marker"];
                        RenderSnapshot snap;
                        snap.DrawingEnabled = m.value("drawingEnabled", true);
                        snap.Opacity = m.value("opacity", 1.0f);
                        snap.TextEnabled = m.value("textEnabled", true);
                        snap.TextFormat = Utf8ToUtf16(m.value("textFormat", "{username} | {machinename}"));
                        snap.TextSize = m.value("textSize", 24.0f);
                        snap.TextOpacity = m.value("textOpacity", 0.5f);
                        snap.TextAngleDeg = (float)m.value("textAngle", -30);
                        snap.TextSpacingEnabled = m.value("textSpacingEnabled", true);
                        snap.TextSpacingX = m.value("textSpacingX", 400.0f);
                        snap.TextSpacingY = m.value("textSpacingY", 250.0f);
                        snap.TextRows = m.value("textRows", 5);
                        snap.TextCols = m.value("textCols", 4);
                        snap.TextColor1 = Utf8ToUtf16(m.value("textColor1", "#FFFFFF"));
                        snap.TextColor2 = Utf8ToUtf16(m.value("textColor2", "#FFFFFF"));
                        { std::lock_guard<std::mutex> lock(g_SnapMutex); g_Snapshot = snap; }
                    }
                } catch (...) {}
                accumulator.erase(0, pos + 1);
            }
        }
        CloseHandle(hPipe);
        Sleep(1000);
    }
    return 0;
}

// --- Watchdog Thread: Monitor Service Presence ---
DWORD WINAPI WatchdogThread(LPVOID) {
    while (!g_bUnloading.load()) {
        Sleep(3000);
        if (!IsAgileMarkPresent()) {
            InitiateHardUnload();
            return 0; 
        }
    }
    return 0;
}

// --- Adaptive Drawing Engine: Main Rendering Logic ---
void DrawAdaptive(IMemInputPin* pPin, IMediaSample* pSample) {
    g_activeCalls++;
    if (g_bUnloading.load()) { g_activeCalls--; return; }

    BYTE* pBuffer = NULL; 
    if (FAILED(pSample->GetPointer(&pBuffer))) { g_activeCalls--; return; }
    long bufferLen = pSample->GetActualDataLength();

    StreamFormat fmt; { std::lock_guard<std::mutex> lock(g_CacheMutex); fmt = g_PinCache[pPin]; }

    AM_MEDIA_TYPE* pmt = NULL;
    if (SUCCEEDED(pSample->GetMediaType(&pmt)) && pmt != NULL) {
        GetVideoInfo(pmt, fmt.w, fmt.h, fmt.bpp);
        fmt.isValid = (fmt.w > 0 && fmt.h > 0);
        { std::lock_guard<std::mutex> lock(g_CacheMutex); g_PinCache[pPin] = fmt; }
        if (pmt->cbFormat != 0) CoTaskMemFree(pmt->pbFormat); if (pmt->pUnk != NULL) pmt->pUnk->Release(); CoTaskMemFree(pmt);
    }

    // Direct Pin Query if cache is empty
    if (!fmt.isValid) {
        IPin* pIPin = NULL;
        if (SUCCEEDED(pPin->QueryInterface(IID_IPin, (void**)&pIPin))) {
            AM_MEDIA_TYPE mt;
            if (SUCCEEDED(pIPin->ConnectionMediaType(&mt))) {
                GetVideoInfo(&mt, fmt.w, fmt.h, fmt.bpp);
                fmt.isValid = (fmt.w > 0 && fmt.h > 0);
                { std::lock_guard<std::mutex> lock(g_CacheMutex); g_PinCache[pPin] = fmt; }
                if (mt.cbFormat != 0) CoTaskMemFree(mt.pbFormat); if (mt.pUnk != NULL) mt.pUnk->Release();
            }
            pIPin->Release();
        }
    }

    if (!fmt.isValid) { g_activeCalls--; return; }

    try {
        int stride = fmt.w * (fmt.bpp / 8); if (stride * fmt.h > bufferLen) { g_activeCalls--; return; }
        Gdiplus::PixelFormat gdiPf = (fmt.bpp == 24) ? PixelFormat24bppRGB : PixelFormat32bppRGB;
        Gdiplus::Bitmap bmp(fmt.w, fmt.h, stride, gdiPf, pBuffer);
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // 1. DRAW DEBUG TEXT (Fixed red AGILEMARK)
        {
            Gdiplus::FontFamily ff(L"Arial Black");
            Gdiplus::Font font(&ff, (float)fmt.h / 10.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat sf; sf.SetAlignment(Gdiplus::StringAlignmentCenter); sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::SolidBrush shadow(Gdiplus::Color(150, 0, 0, 0));
            Gdiplus::SolidBrush textBr(Gdiplus::Color(255, 255, 0, 0));
            g.DrawString(L"AGILEMARK", -1, &font, Gdiplus::RectF(2, 2, (float)fmt.w, (float)fmt.h), &sf, &shadow);
            g.DrawString(L"AGILEMARK", -1, &font, Gdiplus::RectF(0, 0, (float)fmt.w, (float)fmt.h), &sf, &textBr);
        }

        // 2. DRAW DYNAMIC WATERMARK (Grid logic)
        RenderSnapshot snap; { std::lock_guard<std::mutex> lock(g_SnapMutex); snap = g_Snapshot; }
        if (snap.DrawingEnabled && snap.TextEnabled) {
            std::wstring text = ExpandMacros(snap.TextFormat);
            Gdiplus::FontFamily ff(L"Segoe UI");
            Gdiplus::Font font(&ff, snap.TextSize > 5 ? snap.TextSize : 24.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            float effAlpha = snap.Opacity * snap.TextOpacity;
            Gdiplus::SolidBrush br1(ParseHexColor(snap.TextColor1, effAlpha));
            Gdiplus::SolidBrush br2(ParseHexColor(snap.TextColor2, effAlpha));

            int rows = snap.TextRows > 0 ? snap.TextRows : 1;
            int cols = snap.TextCols > 0 ? snap.TextCols : 1;
            float sx = snap.TextSpacingEnabled ? (snap.TextSpacingX > 10 ? snap.TextSpacingX : 400) : (float)fmt.w / cols;
            float sy = snap.TextSpacingEnabled ? (snap.TextSpacingY > 10 ? snap.TextSpacingY : 250) : (float)fmt.h / rows;

            for (int r = 0; r < rows; r++) {
                for (int c = 0; c < cols; c++) {
                    Gdiplus::GraphicsState state = g.Save();
                    g.TranslateTransform(c * sx + sx / 2, r * sy + sy / 2);
                    g.RotateTransform(snap.TextAngleDeg);
                    Gdiplus::RectF layout; g.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), &layout);
                    Gdiplus::SolidBrush& br = ((r + c) % 2 == 0) ? br1 : br2;
                    g.DrawString(text.c_str(), -1, &font, Gdiplus::PointF(-layout.Width/2, -layout.Height/2), &br);
                    g.Restore(state);
                }
            }
        }
    } catch (...) {}

    g_activeCalls--;
}

// --- Hooks: DirectShow Interface Hooking ---
HRESULT STDMETHODCALLTYPE HookedReceive(IMemInputPin* pPin, IMediaSample* pSample) {
    if (g_bUnloading.load()) return g_origReceive(pPin, pSample);
    DrawAdaptive(pPin, pSample);
    return g_origReceive(pPin, pSample);
}

void HookFilterPins(IBaseFilter* pFilter) {
    if (g_bUnloading.load()) return;
    IEnumPins* pEnum = NULL;
    if (SUCCEEDED(pFilter->EnumPins(&pEnum))) {
        IPin* pPin = NULL;
        while (pEnum->Next(1, &pPin, NULL) == S_OK) {
            IMemInputPin* pInputPin = NULL;
            if (SUCCEEDED(pPin->QueryInterface(IID_IMemInputPin, (void**)&pInputPin))) {
                void** vtable = *(void***)pInputPin;
                MH_CreateHook(vtable[6], &HookedReceive, (LPVOID*)&g_origReceive);
                MH_EnableHook(vtable[6]);
                pInputPin->Release();
            }
            pPin->Release();
        }
        pEnum->Release();
    }
}

HRESULT STDMETHODCALLTYPE HookedAddFilter(IFilterGraph* pGraph, IBaseFilter* pFilter, LPCWSTR pName) {
    if (g_bUnloading.load()) return g_origAddFilter(pGraph, pFilter, pName);
    HookFilterPins(pFilter);
    return g_origAddFilter(pGraph, pFilter, pName);
}

HRESULT WINAPI HookedCoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv) {
    HRESULT hr = g_origCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    if (!g_bUnloading.load() && SUCCEEDED(hr) && ppv && *ppv) {
        if (rclsid == CLSID_FilterGraph || rclsid == CLSID_FilterGraphNoThread) {
            IFilterGraph* pGraph = (IFilterGraph*)*ppv;
            void** vtable = *(void***)pGraph;
            MH_CreateHook(vtable[3], &HookedAddFilter, (LPVOID*)&g_origAddFilter);
            MH_EnableHook(vtable[3]);
        }
    }
    return hr;
}

// --- Entry Point: Exported function to start watching ---
extern "C" __declspec(dllexport) DWORD WINAPI StartWatch(LPVOID lp) {
    DebugLog::initialize();
    DebugLog::log("[WebcamDLL] StartWatch");
    
    // Initial default snapshot for immediate visibility
    {
        std::lock_guard<std::mutex> lock(g_SnapMutex);
        g_Snapshot.DrawingEnabled = true; g_Snapshot.TextEnabled = true;
        g_Snapshot.TextFormat = L"AGILEMARK DYNAMIC"; g_Snapshot.TextSize = 24.0f;
        g_Snapshot.TextOpacity = 0.5f; g_Snapshot.Opacity = 1.0f;
        g_Snapshot.TextColor1 = L"#FFFFFF"; g_Snapshot.TextColor2 = L"#FFFFFF";
    }

    if (g_gdiplusToken == 0) {
        Gdiplus::GdiplusStartupInput gsi; Gdiplus::GdiplusStartup(&g_gdiplusToken, &gsi, NULL);
    }
    if (MH_Initialize() == MH_OK) {
        MH_CreateHookApi(L"ole32.dll", "CoCreateInstance", &HookedCoCreateInstance, (LPVOID*)&g_origCoCreateInstance);
        MH_EnableHook(MH_ALL_HOOKS);
    }
    CreateThread(NULL, 0, IpcThread, NULL, 0, NULL);
    CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
    return 0;
}

// --- DllMain: DLL Lifecycle ---
BOOL APIENTRY DllMain(HMODULE hMod, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hMod); g_hModule = hMod; }
    return TRUE;
}
