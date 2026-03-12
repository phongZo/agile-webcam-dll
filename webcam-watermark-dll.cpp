#include "pch.h"
#include <windows.h>
#include <string>
#include <map>
#include <mutex>
#include <dshow.h>
#include <dvdmedia.h>
#include "DebugLog.h"
#include "../packages/minhook.1.3.3/lib/native/include/MinHook.h"

// --- LINKER LIBRARIES ---
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

#ifdef _WIN64
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x64-v141-mt.lib")
#else
    #pragma comment(lib, "../packages/minhook.1.3.3/lib/native/lib/libMinHook-x86-v141-mt.lib")
#endif

// --- COM Interface Typedefs ---
typedef HRESULT(WINAPI* PCoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
PCoCreateInstance g_origCoCreateInstance = NULL;

typedef HRESULT(STDMETHODCALLTYPE* PAddFilter)(IFilterGraph*, IBaseFilter*, LPCWSTR);
PAddFilter g_origAddFilter = NULL;

typedef HRESULT(STDMETHODCALLTYPE* PReceive)(IMemInputPin*, IMediaSample*);
PReceive g_origReceive = NULL;

// --- Adaptive Format Storage ---
struct StreamFormat {
    int w = 0;
    int h = 0;
    int bpp = 0;
    bool isValid = false;
};
std::map<IMemInputPin*, StreamFormat> g_PinCache;
std::mutex g_CacheMutex;

// --- Helper: Giải mã thông tin Video từ Media Type ---
bool GetVideoInfo(AM_MEDIA_TYPE* pmt, int& w, int& h, int& bpp) {
    if (!pmt) return false;
    if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
        w = vih->bmiHeader.biWidth;
        h = abs(vih->bmiHeader.biHeight);
        bpp = vih->bmiHeader.biBitCount;
        return true;
    }
    if (pmt->formattype == FORMAT_VideoInfo2 && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        VIDEOINFOHEADER2* vih2 = (VIDEOINFOHEADER2*)pmt->pbFormat;
        w = vih2->bmiHeader.biWidth;
        h = abs(vih2->bmiHeader.biHeight);
        bpp = vih2->bmiHeader.biBitCount;
        return true;
    }
    return false;
}

// --- Adaptive Drawing Engine ---
void DrawAdaptive(IMemInputPin* pPin, IMediaSample* pSample) {
    BYTE* pBuffer = NULL;
    if (FAILED(pSample->GetPointer(&pBuffer))) return;
    long bufferLen = pSample->GetActualDataLength();

    StreamFormat fmt;
    {
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        fmt = g_PinCache[pPin];
    }

    // Nếu chưa có format hoặc có sự thay đổi format động (Dynamic Format Change)
    AM_MEDIA_TYPE* pmt = NULL;
    if (SUCCEEDED(pSample->GetMediaType(&pmt)) && pmt != NULL) {
        GetVideoInfo(pmt, fmt.w, fmt.h, fmt.bpp);
        fmt.isValid = (fmt.w > 0 && fmt.h > 0);
        // Cập nhật cache
        std::lock_guard<std::mutex> lock(g_CacheMutex);
        g_PinCache[pPin] = fmt;
        
        // Giải phóng pmt theo chuẩn COM
        if (pmt->cbFormat != 0) CoTaskMemFree(pmt->pbFormat);
        if (pmt->pUnk != NULL) pmt->pUnk->Release();
        CoTaskMemFree(pmt);
        DebugLog::log("[WebcamDLL] Dynamic Format Change Detected: " + std::to_string(fmt.w) + "x" + std::to_string(fmt.h));
    }

    // Nếu vẫn chưa có thông tin trong cache, thử hỏi trực tiếp Pin
    if (!fmt.isValid) {
        IPin* pIPin = NULL;
        if (SUCCEEDED(pPin->QueryInterface(IID_IPin, (void**)&pIPin))) {
            AM_MEDIA_TYPE mt;
            if (SUCCEEDED(pIPin->ConnectionMediaType(&mt))) {
                GetVideoInfo(&mt, fmt.w, fmt.h, fmt.bpp);
                fmt.isValid = (fmt.w > 0 && fmt.h > 0);
                std::lock_guard<std::mutex> lock(g_CacheMutex);
                g_PinCache[pPin] = fmt;
                
                if (mt.cbFormat != 0) CoTaskMemFree(mt.pbFormat);
                if (mt.pUnk != NULL) mt.pUnk->Release();
                DebugLog::log("[WebcamDLL] Initial Format Detected: " + std::to_string(fmt.w) + "x" + std::to_string(fmt.h));
            }
            pIPin->Release();
        }
    }

    if (!fmt.isValid) return;

    try {
        Gdiplus::PixelFormat gdiPf = (fmt.bpp == 24) ? PixelFormat24bppRGB : PixelFormat32bppRGB;
        int stride = fmt.w * (fmt.bpp / 8);
        
        // Kiểm tra an toàn: Tránh vẽ tràn buffer nếu tính toán sai lệch
        if (stride * fmt.h > bufferLen) return;

        Gdiplus::Bitmap bmp(fmt.w, fmt.h, stride, gdiPf, pBuffer);
        Gdiplus::Graphics g(&bmp);
        
        Gdiplus::FontFamily ff(L"Arial Black");
        float fontSize = (float)fmt.h / 8.0f;
        Gdiplus::Font font(&ff, fontSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        Gdiplus::SolidBrush shadow(Gdiplus::Color(150, 0, 0, 0));
        Gdiplus::SolidBrush text(Gdiplus::Color(200, 255, 0, 0));
        
        g.DrawString(L"AGILEMARK", -1, &font, Gdiplus::RectF(2, 2, (float)fmt.w, (float)fmt.h), &sf, &shadow);
        g.DrawString(L"AGILEMARK", -1, &font, Gdiplus::RectF(0, 0, (float)fmt.w, (float)fmt.h), &sf, &text);
    } catch (...) {}
}

HRESULT STDMETHODCALLTYPE HookedReceive(IMemInputPin* pPin, IMediaSample* pSample) {
    DrawAdaptive(pPin, pSample);
    return g_origReceive(pPin, pSample);
}

void HookFilterPins(IBaseFilter* pFilter) {
    IEnumPins* pEnum = NULL;
    if (SUCCEEDED(pFilter->EnumPins(&pEnum))) {
        IPin* pPin = NULL;
        while (pEnum->Next(1, &pPin, NULL) == S_OK) {
            IMemInputPin* pInputPin = NULL;
            if (SUCCEEDED(pPin->QueryInterface(IID_IMemInputPin, (void**)&pInputPin))) {
                void** vtable = *(void***)pInputPin;
                if (MH_CreateHook(vtable[6], &HookedReceive, (LPVOID*)&g_origReceive) == MH_OK) {
                    MH_EnableHook(vtable[6]);
                } else {
                    MH_EnableHook(vtable[6]);
                }
                pInputPin->Release();
            }
            pPin->Release();
        }
        pEnum->Release();
    }
}

HRESULT STDMETHODCALLTYPE HookedAddFilter(IFilterGraph* pGraph, IBaseFilter* pFilter, LPCWSTR pName) {
    HookFilterPins(pFilter);
    return g_origAddFilter(pGraph, pFilter, pName);
}

HRESULT WINAPI HookedCoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid, LPVOID* ppv) {
    HRESULT hr = g_origCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
    if (SUCCEEDED(hr) && ppv && *ppv) {
        if (rclsid == CLSID_FilterGraph || rclsid == CLSID_FilterGraphNoThread) {
            IFilterGraph* pGraph = (IFilterGraph*)*ppv;
            void** vtable = *(void***)pGraph;
            if (MH_CreateHook(vtable[3], &HookedAddFilter, (LPVOID*)&g_origAddFilter) == MH_OK) {
                MH_EnableHook(vtable[3]);
            }
        }
    }
    return hr;
}

extern "C" __declspec(dllexport) DWORD WINAPI StartWatch(LPVOID lp) {
    DebugLog::initialize();
    DebugLog::log("[WebcamDLL] StartWatch version 25.1 (Adaptive Stream Hunter)");
    
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    if (MH_Initialize() == MH_OK) {
        MH_CreateHookApi(L"ole32.dll", "CoCreateInstance", &HookedCoCreateInstance, (LPVOID*)&g_origCoCreateInstance);
        MH_EnableHook(MH_ALL_HOOKS);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID res) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hMod);
    return TRUE;
}
