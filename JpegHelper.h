#pragma once
#include <windows.h>
#include <wincodec.h>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

class JpegHelper {
public:
    static bool DecompressMJPG(BYTE* pInput, DWORD cbInput, int& width, int& height, std::vector<BYTE>& outRgba);
    static bool CompressMJPG(BYTE* pRgba, int width, int height, std::vector<BYTE>& outJpeg, int quality = 85);
};
