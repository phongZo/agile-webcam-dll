#pragma once

#include <windows.h>
#include <cstring>

inline bool IsMjpgSubtype(REFGUID subtype) {
    return subtype.Data1 == 0x47504A4D &&
        subtype.Data2 == 0x0000 &&
        subtype.Data3 == 0x0010 &&
        subtype.Data4[0] == 0x80 &&
        subtype.Data4[1] == 0x00 &&
        subtype.Data4[2] == 0x00 &&
        subtype.Data4[3] == 0xAA &&
        subtype.Data4[4] == 0x00 &&
        subtype.Data4[5] == 0x38 &&
        subtype.Data4[6] == 0x9B &&
        subtype.Data4[7] == 0x71;
}

inline bool HasAgileMarkJpegSignature(const BYTE* pData, DWORD length) {
    static const char kSignature[] = "AgileMark";
    constexpr DWORD kSignatureLen = sizeof(kSignature) - 1;
    constexpr DWORD kSignatureOffset = 6;
    constexpr DWORD kMinTaggedLength = kSignatureOffset + kSignatureLen;

    if (!pData || length < kMinTaggedLength) return false;
    if (pData[0] != 0xFF || pData[1] != 0xD8) return false;
    if (pData[2] != 0xFF || pData[3] != 0xFE) return false;
    return std::memcmp(pData + kSignatureOffset, kSignature, kSignatureLen) == 0;
}
