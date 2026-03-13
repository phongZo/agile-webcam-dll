#pragma once
#include "RenderSnapshot.h"
#include <cstdlib>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dcommon.h>
#include <dwrite.h>
#include <string>

class RendererBase
{
public:
	virtual ~RendererBase() = default;

	virtual void ApplySettings(const RenderSnapshot& snap) = 0;

	virtual void DrawOverlay(ID2D1RenderTarget* rt, IDWriteFactory* dwFactory, const D2D1_SIZE_F& sz) = 0;

protected:
	static D2D1::ColorF ParseColor(const std::wstring& hexNoAlpha, float opacity /*0..1*/)
	{
		unsigned r = 0, g = 0, b = 0;
		if (hexNoAlpha.size() == 7 && hexNoAlpha[0] == L'#')
		{
			wchar_t* end = nullptr;
			unsigned val = wcstoul(hexNoAlpha.c_str() + 1, &end, 16);
			r = (val >> 16) & 0xFF;
			g = (val >> 8) & 0xFF;
			b = (val) & 0xFF;
		}
		return D2D1::ColorF(
			r / 255.0f, g / 255.0f, b / 255.0f,
			opacity
		);
	}
};
