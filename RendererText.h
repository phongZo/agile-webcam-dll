#pragma once
#include "RendererBase.h"
#include "RenderSnapshot.h"
#include <d2d1.h>
#include <d2d1helper.h>
#include <dcommon.h>
#include <dwrite.h>
#include <string>
#include <wrl/client.h>

class RendererText : public RendererBase
{
public:
	RendererText() = default;

	void ApplySettings(const RenderSnapshot& snap) override;
	void DrawOverlay(ID2D1RenderTarget* rt, IDWriteFactory* dwFactory, const D2D1_SIZE_F& sz) override;

private:
	RenderSnapshot   m_snap{};
	D2D1::ColorF     m_col1{ D2D1::ColorF(0, 0) };
	D2D1::ColorF     m_col2{ D2D1::ColorF(1, 1) };

	Microsoft::WRL::ComPtr<IDWriteTextFormat> m_fmt;

	// Expand macros: {UserName}, {MachineName}, {ShortDate}, {ShortTime}, {CustomDateTime}...
	std::wstring ExpandTextMacros(const std::wstring& fmt, const std::wstring& ts);

	void DrawOne(ID2D1RenderTarget* rt,
		const std::wstring& text,
		D2D1_POINT_2F origin,
		const D2D1::ColorF& col);

	void DrawOneWrapped(ID2D1RenderTarget* rt,
		IDWriteFactory* dwFactory,
		const std::wstring& text,
		D2D1_POINT_2F origin,
		const D2D1::ColorF& col,
		float cellWidth);

	D2D1_SIZE_F MeasureUnwrapped(IDWriteFactory* dwFactory, const std::wstring& text);

	D2D1_SIZE_F MeasureWrapped(IDWriteFactory* dwFactory, const std::wstring& text, float width);

	// Log helper
	static std::string wide_to_utf8(const std::wstring& w);
};
