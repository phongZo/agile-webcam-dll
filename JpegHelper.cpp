#include "pch.h"
#include "JpegHelper.h"
#include <wincodec.h>
#include <vector>
#include <comdef.h>
#include <shlwapi.h>
#include <mutex>
#include "DebugLog.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

static IWICImagingFactory* g_pWICFactory = nullptr;
static std::mutex g_wicMutex;

static IWICImagingFactory* GetWICFactory() {
    std::lock_guard<std::mutex> lock(g_wicMutex);
    if (!g_pWICFactory) {
        // Try to create the factory. We assume COM is initialized.
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_pWICFactory));
        if (FAILED(hr)) {
            // If it failed because of COM not initialized, try to initialize it here for this thread
            if (hr == CO_E_NOTINITIALIZED) {
                CoInitializeEx(NULL, COINIT_MULTITHREADED);
                hr = CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_pWICFactory));
            }
            if (FAILED(hr)) {
                char buf[128];
                sprintf_s(buf, "[WebcamDLL][WIC] Failed to create Factory: 0x%08X", hr);
                DebugLog::log(buf);
            }
        }
    }
    return g_pWICFactory;
}

bool JpegHelper::DecompressMJPG(BYTE* pInput, DWORD cbInput, int& width, int& height, std::vector<BYTE>& outRgba) {
    if (!pInput || cbInput < 10) return false; // Basic check

    IWICImagingFactory* pFactory = GetWICFactory();
    if (!pFactory) return false;

    IWICStream* pStream = nullptr;
    if (FAILED(pFactory->CreateStream(&pStream))) return false;
    
    HRESULT hr = pStream->InitializeFromMemory(pInput, cbInput);
    if (FAILED(hr)) {
        pStream->Release();
        return false;
    }

    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pFactory->CreateDecoderFromStream(pStream, NULL, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) {
        pStream->Release();
        return false;
    }

    IWICBitmapFrameDecode* pFrame = nullptr;
    if (FAILED(pDecoder->GetFrame(0, &pFrame))) {
        pDecoder->Release();
        pStream->Release();
        return false;
    }

    UINT w, h;
    pFrame->GetSize(&w, &h);
    width = (int)w;
    height = (int)h;

    IWICFormatConverter* pConverter = nullptr;
    if (FAILED(pFactory->CreateFormatConverter(&pConverter))) {
        pFrame->Release();
        pDecoder->Release();
        pStream->Release();
        return false;
    }

    hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) {
        pConverter->Release();
        pFrame->Release();
        pDecoder->Release();
        pStream->Release();
        return false;
    }

    try {
        outRgba.resize((size_t)w * h * 4);
        hr = pConverter->CopyPixels(NULL, w * 4, (UINT)outRgba.size(), outRgba.data());
    } catch (...) {
        hr = E_OUTOFMEMORY;
    }
    
    pConverter->Release();
    pFrame->Release();
    pDecoder->Release();
    pStream->Release();
    
    return SUCCEEDED(hr);
}

bool JpegHelper::CompressMJPG(BYTE* pRgba, int width, int height, std::vector<BYTE>& outJpeg, int quality) {
    if (!pRgba || width <= 0 || height <= 0) return false;

    IWICImagingFactory* pFactory = GetWICFactory();
    if (!pFactory) return false;

    IStream* pMemStream = SHCreateMemStream(NULL, 0);
    if (!pMemStream) return false;

    IWICBitmapEncoder* pEncoder = nullptr;
    if (FAILED(pFactory->CreateEncoder(GUID_ContainerFormatJpeg, NULL, &pEncoder))) {
        pMemStream->Release(); return false;
    }

    if (FAILED(pEncoder->Initialize(pMemStream, WICBitmapEncoderNoCache))) {
        pEncoder->Release(); pMemStream->Release(); return false;
    }

    IWICBitmapFrameEncode* pFrame = nullptr;
    IPropertyBag2* pPropertyBag = nullptr;
    if (FAILED(pEncoder->CreateNewFrame(&pFrame, &pPropertyBag))) {
        pEncoder->Release(); pMemStream->Release(); return false;
    }

    if (pPropertyBag) {
        PROPBAG2 options[2] = { 0 };
        VARIANT vars[2];
        
        options[0].pstrName = (LPOLESTR)L"ImageQuality";
        VariantInit(&vars[0]); vars[0].vt = VT_R4; vars[0].fltVal = (float)quality / 100.0f;

        options[1].pstrName = (LPOLESTR)L"JpegYCrCbSubsampling";
        VariantInit(&vars[1]); vars[1].vt = VT_UI1; vars[1].bVal = 0x02; // Force 4:2:2

        pPropertyBag->Write(2, options, vars);
    }

    HRESULT hr = pFrame->Initialize(pPropertyBag);
    if (pPropertyBag) pPropertyBag->Release();

    if (FAILED(hr)) {
        pFrame->Release(); pEncoder->Release(); pMemStream->Release(); return false;
    }

    pFrame->SetSize(width, height);
    pFrame->SetResolution(96.0, 96.0); 
    
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    pFrame->SetPixelFormat(&format);

    std::vector<BYTE> bgr24;
    try {
        bgr24.resize((size_t)width * height * 3);
        BYTE* pS = pRgba; BYTE* pD = bgr24.data();
        for (int i = 0; i < width * height; i++) {
            pD[0] = pS[0]; pD[1] = pS[1]; pD[2] = pS[2];
            pS += 4; pD += 3;
        }
        hr = pFrame->WritePixels(height, width * 3, (UINT)bgr24.size(), bgr24.data());
    } catch (...) { hr = E_OUTOFMEMORY; }

    if (SUCCEEDED(hr)) {
        pFrame->Commit();
        pEncoder->Commit();

        LARGE_INTEGER liZero = { 0 };
        ULARGE_INTEGER pPos;
        pMemStream->Seek(liZero, STREAM_SEEK_CUR, &pPos);
        
        try {
            outJpeg.resize((size_t)pPos.QuadPart);
            pMemStream->Seek(liZero, STREAM_SEEK_SET, NULL);
            ULONG read = 0;
            pMemStream->Read(outJpeg.data(), (ULONG)outJpeg.size(), &read);

            // INSERT SECRET SIGNATURE (FF FE ... AgileMark) to avoid double processing
            // We find the SOS (FF DA) and insert before it, or just append after SOI
            if (outJpeg.size() > 10) {
                const char* sig = "AgileMark";
                int sigLen = (int)strlen(sig);
                std::vector<BYTE> tagged;
                tagged.push_back(0xFF); tagged.push_back(0xD8); // SOI
                tagged.push_back(0xFF); tagged.push_back(0xFE); // COM Marker
                tagged.push_back(0x00); tagged.push_back((BYTE)(sigLen + 2)); // Length
                for(int i=0; i<sigLen; i++) tagged.push_back(sig[i]);
                tagged.insert(tagged.end(), outJpeg.begin() + 2, outJpeg.end());
                outJpeg = std::move(tagged);
            }
        } catch (...) { outJpeg.clear(); }
    }

    pFrame->Release();
    pEncoder->Release();
    pMemStream->Release();
    return !outJpeg.empty();
}
