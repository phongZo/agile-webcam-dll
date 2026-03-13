#include "pch.h"
#include "DebugLog.h"
#include "RendererText.h"
#include "RenderSnapshot.h"
#include <cmath>
#include <cstdio>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dcommon.h>
#include <dwrite.h>
#include <Lmcons.h> 
#include <string>

#include <libloaderapi.h>
#include <Lmcons.h>    // UNLEN
#include <minwinbase.h>
#include <stringapiset.h>
#include <sysinfoapi.h>
#include <utility>
#include <Windows.h>
#include <WinNls.h>
#include <wrl/client.h>

#pragma comment(lib, "Secur32.lib")
// ---- Compatibility shim for old SDKs ----
#ifndef EXTENDED_NAME_FORMAT
typedef enum {
	NameUnknown = 0,
	NameFullyQualifiedDN = 1,
	NameSamCompatible = 2,
	NameDisplay = 3,
	NameUniqueId = 6,
	NameCanonical = 7,
	NameUserPrincipal = 8,  // user@domain
	NameCanonicalEx = 9,
	NameServicePrincipal = 10,
	NameDnsDomain = 12
} EXTENDED_NAME_FORMAT, * PEXTENDED_NAME_FORMAT;
#endif

#ifndef NameSamCompatible
#define NameSamCompatible ((EXTENDED_NAME_FORMAT)2)
#endif
#ifndef NameUserPrincipal
#define NameUserPrincipal ((EXTENDED_NAME_FORMAT)8)
#endif
using Microsoft::WRL::ComPtr;

//===================== small utils =====================
static inline float maxf(float a, float b) { return (a > b) ? a : b; }
static inline float minf(float a, float b) { return (a < b) ? a : b; }
static inline float clamp01(float v) { return maxf(0.0f, minf(1.0f, v)); }

static std::wstring ToLowerCopy(std::wstring t) {
	for (auto& c : t) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
	return t;
}

static void ReplaceAllCI(std::wstring& inout,
	const std::wstring& needle,
	const std::wstring& repl)
{
	if (needle.empty()) return;
	std::wstring lower = ToLowerCopy(inout);
	const std::wstring lneedle = ToLowerCopy(needle);
	size_t pos = 0;
	while (true) {
		pos = lower.find(lneedle, pos);
		if (pos == std::wstring::npos) break;
		inout.replace(pos, lneedle.size(), repl);
		lower.replace(pos, lneedle.size(), repl);
		pos += repl.size();
	}
}

static D2D1::ColorF ParseHexColor(const std::wstring& hex, float alphaMul = 1.0f)
{
	// accept "#RRGGBB" or "RRGGBB"
	std::wstring s = hex;
	if (!s.empty() && s[0] == L'#') s.erase(0, 1);
	if (s.size() != 6) return D2D1::ColorF(0.f, 0.f, 0.f, alphaMul);

	auto hex2i = [](wchar_t c)->int {
		if (c >= L'0' && c <= L'9') return c - L'0';
		if (c >= L'a' && c <= L'f') return c - L'a' + 10;
		if (c >= L'A' && c <= L'F') return c - L'A' + 10;
		return 0;
		};
	int r = hex2i(s[0]) * 16 + hex2i(s[1]);
	int g = hex2i(s[2]) * 16 + hex2i(s[3]);
	int b = hex2i(s[4]) * 16 + hex2i(s[5]);
	return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, alphaMul);
}
static bool GetNameExSafe(EXTENDED_NAME_FORMAT fmt, std::wstring& out) {
	out.clear();

	using GetUserNameExW_t = BOOLEAN(WINAPI*)(EXTENDED_NAME_FORMAT, LPWSTR, PULONG);

	HMODULE h = ::GetModuleHandleW(L"secur32.dll");
	if (!h) h = ::LoadLibraryW(L"secur32.dll");
	if (!h) return false;

	auto pGetUserNameExW = reinterpret_cast<GetUserNameExW_t>(
		::GetProcAddress(h, "GetUserNameExW"));
	if (!pGetUserNameExW) return false;

	ULONG sz = 0;
	if (!pGetUserNameExW(fmt, nullptr, &sz) || sz == 0) return false;

	std::wstring buf(sz, L'\0');
	if (!pGetUserNameExW(fmt, &buf[0], &sz) || sz == 0) return false;

	buf.resize(sz);
	while (!buf.empty() && buf.back() == L'\0') buf.pop_back();

	out = std::move(buf);
	return true;
}

static std::wstring GetOsUserNameSimple()
{
	// 1) UPN: user@domain
	{
		std::wstring upn;
		if (GetNameExSafe(NameUserPrincipal, upn) && !upn.empty()) {
			const size_t at = upn.find(L'@');
			return (at != std::wstring::npos) ? upn.substr(0, at) : upn;
		}
	}

	// 2) SAM: DOMAIN\user 
	{
		std::wstring sam;
		if (GetNameExSafe(NameSamCompatible, sam) && !sam.empty()) {
			const size_t bs = sam.rfind(L'\\');
			return (bs != std::wstring::npos) ? sam.substr(bs + 1) : sam;
		}
	}

	// 3) Fallback: GetUserNameW
	{
		wchar_t user[UNLEN + 2] = L"";
		DWORD cch = (DWORD)(sizeof(user) / sizeof(user[0])); // _countof(user)
		if (GetUserNameW(user, &cch) && cch > 0) {
			return std::wstring(user);
		}
	}

	return L"";
}


//===================== class impl =====================

void RendererText::ApplySettings(const RenderSnapshot& snap)
{
	m_snap = snap;
	m_fmt.Reset();
	wchar_t pre[256];
	swprintf_s(pre,
		L"[DComp][TEXT] ApplySettings: draw=%d opacity=%.2f text(on=%d size=%.1f angle=%.1f textOpa=%.2f fmtLen=%zu) grid(on=%d r=%d c=%d sp(%d,%d))",
		(int)m_snap.DrawingEnabled,
		m_snap.Opacity,
		(int)m_snap.TextEnabled,
		m_snap.TextSize,
		m_snap.TextAngleDeg,
		m_snap.TextOpacity,
		(size_t)m_snap.TextFormat.size(),
		(int)m_snap.GridEnabled,
		m_snap.GridRows, m_snap.GridCols,
		m_snap.GridSpacingX, m_snap.GridSpacingY
	);
	DebugLog::log(wide_to_utf8(pre).c_str());
}

void RendererText::DrawOverlay(ID2D1RenderTarget* rt, IDWriteFactory* dwFactory, const D2D1_SIZE_F& sz)
{
	if (!rt || !dwFactory) { DebugLog::log("[DComp][DRAW] missing RT/factory"); return; }

	const float effAlpha = clamp01(m_snap.TextOpacity) * clamp01(m_snap.Opacity);
	/*char pre[256];
	sprintf_s(pre, "[DComp][DRAW] Begin draw=%d text=%d textOpa=%.3f globOpa=%.3f eff=%.3f",
		(int)m_snap.DrawingEnabled, (int)m_snap.TextEnabled, m_snap.TextOpacity, m_snap.Opacity, effAlpha);
	DebugLog::log(pre);*/

	if (!m_snap.DrawingEnabled) { DebugLog::log("[DComp][DRAW] Drawing disabled -> skip"); return; }
	if (!m_snap.TextEnabled) { DebugLog::log("[DComp][DRAW] Text disabled -> skip"); return; }

	// Expand text by TextFormat
	SYSTEMTIME st{}; GetLocalTime(&st);
	wchar_t tsBuf[64];

	swprintf_s(tsBuf, L"%02d/%02d/%04d %02d:%02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond);
	const std::wstring baseTs = tsBuf;

	const std::wstring content = ExpandTextMacros(m_snap.TextFormat, baseTs);

	if (!m_fmt) {
		ComPtr<IDWriteTextFormat> fmt;
		HRESULT hr = dwFactory->CreateTextFormat(
			L"Segoe UI",
			nullptr,
			DWRITE_FONT_WEIGHT_BOLD,
			DWRITE_FONT_STYLE_NORMAL,
			DWRITE_FONT_STRETCH_NORMAL,
			m_snap.TextSize,
			L"vi-VN",
			&fmt
		);
		if (FAILED(hr)) { DebugLog::log("[DComp][DRAW] CreateTextFormat failed"); return; }
		fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
		fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
		m_fmt = fmt;
	}

	const D2D1::ColorF colA = ParseHexColor(m_snap.TextColor1, effAlpha);
	const D2D1::ColorF colB = ParseHexColor(m_snap.TextColor2, effAlpha);

	float stepX = 0, stepY = 0;
	float padX = 0, padY = 0;
	int rows = (m_snap.TextRows > 0) ? m_snap.TextRows : 1;
	int cols = (m_snap.TextCols > 0) ? m_snap.TextCols : 1;

	if (m_snap.TextSpacingEnabled) {
		stepX = (m_snap.TextSpacingX > 10.0f) ? m_snap.TextSpacingX : 10.0f;
		stepY = (m_snap.TextSpacingY > 10.0f) ? m_snap.TextSpacingY : 10.0f;

		if (m_snap.TextAdjustment) {
			const D2D1_SIZE_F raw = MeasureUnwrapped(dwFactory, content);
			const float rad = (m_snap.TextAngleDeg * 3.1415926535f) / 180.f;
			const float sa = (float)fabs(sinf(rad));
			const float ca = (float)fabs(cosf(rad));
			const float rotatedW = raw.width * ca + raw.height * sa;
			const float rotatedH = raw.width * sa + raw.height * ca;

			if (stepX < rotatedW) {
				DebugLog::log("[DComp][DRAW] TextSpacingX adjusted due to rotation");
				stepX = rotatedW + 2.0f;
			}
			if (stepY < rotatedH) {
				DebugLog::log("[DComp][DRAW] TextSpacingY adjusted due to rotation");
				stepY = rotatedH + 2.0f;
			}
		}
	}
	else {
		stepX = (sz.width / (float)cols > 10.0f) ? (sz.width / (float)cols) : 10.0f;
		stepY = (sz.height / (float)rows > 10.0f) ? (sz.height / (float)rows) : 10.0f;

		const D2D1_SIZE_F m = MeasureWrapped(dwFactory, content, stepX);
		const float rad = (m_snap.TextAngleDeg * 3.1415926535f) / 180.f;
		const float sa = (float)fabs(sinf(rad));
		const float ca = (float)fabs(cosf(rad));
		const float rotatedW = m.width * ca + m.height * sa;
		const float rotatedH = m.width * sa + m.height * ca;

		padX = (stepX - rotatedW) * 0.5f; if (padX < 0) padX = 0;
		padY = (stepY - rotatedH) * 0.5f; if (padY < 0) padY = 0;
	}

	bool useA = false;
	for (float y = padY; y < sz.height; y += stepY) {
		for (float x = padX; x < sz.width; x += stepX) {
			useA = !useA;
			const D2D1::ColorF& useCol = useA ? colA : colB;
			const D2D1_POINT_2F p = D2D1::Point2F(x, y);

			if (!m_snap.TextSpacingEnabled) {
				DrawOneWrapped(rt, dwFactory, content, p, useCol, stepX);
			}
			else {
				DrawOne(rt, content, p, useCol);
			}
		}
		if (m_snap.TextSpacingEnabled) useA = !useA;
	}
}

void RendererText::DrawOne(ID2D1RenderTarget* rt,
	const std::wstring& text,
	D2D1_POINT_2F origin,
	const D2D1::ColorF& col)
{
	if (!m_fmt) return;

	const float blur = maxf(0.f, m_snap.TextBlurRadius);
	const float o = (blur <= 0.1f) ? 0.f : blur * 0.25f;

	ComPtr<ID2D1SolidColorBrush> br;
	rt->CreateSolidColorBrush(D2D1::ColorF(col.r, col.g, col.b, col.a), &br);

	const D2D1_RECT_F rc = D2D1::RectF(origin.x, origin.y, origin.x + 4000.0f, origin.y + 400.0f);

	D2D1::Matrix3x2F old;
	rt->GetTransform(&old);
	const auto rot = D2D1::Matrix3x2F::Rotation(m_snap.TextAngleDeg, origin);
	rt->SetTransform(rot * old);

	if (o > 0.0f) {
		ComPtr<ID2D1SolidColorBrush> brShadow;
		rt->CreateSolidColorBrush(D2D1::ColorF(col.r, col.g, col.b, col.a * 0.35f), &brShadow);
		const D2D1_RECT_F off1 = D2D1::RectF(rc.left + o, rc.top + o, rc.right + o, rc.bottom + o);
		const D2D1_RECT_F off2 = D2D1::RectF(rc.left - o, rc.top + o, rc.right - o, rc.bottom + o);
		const D2D1_RECT_F off3 = D2D1::RectF(rc.left + o, rc.top - o, rc.right + o, rc.bottom - o);
		const D2D1_RECT_F off4 = D2D1::RectF(rc.left - o, rc.top - o, rc.right - o, rc.bottom - o);

		rt->DrawTextW(text.c_str(), (UINT32)text.size(), m_fmt.Get(), off1, brShadow.Get());
		rt->DrawTextW(text.c_str(), (UINT32)text.size(), m_fmt.Get(), off2, brShadow.Get());
		rt->DrawTextW(text.c_str(), (UINT32)text.size(), m_fmt.Get(), off3, brShadow.Get());
		rt->DrawTextW(text.c_str(), (UINT32)text.size(), m_fmt.Get(), off4, brShadow.Get());
	}

	rt->DrawTextW(text.c_str(), (UINT32)text.size(), m_fmt.Get(), rc, br.Get());
	rt->SetTransform(&old);
}

void RendererText::DrawOneWrapped(ID2D1RenderTarget* rt,
	IDWriteFactory* dwFactory,
	const std::wstring& text,
	D2D1_POINT_2F origin,
	const D2D1::ColorF& col,
	float cellWidth)
{
	if (!m_fmt) return;

	ComPtr<IDWriteTextLayout> layout;
	dwFactory->CreateTextLayout(
		text.c_str(), (UINT32)text.size(),
		m_fmt.Get(),
		cellWidth, 2000.0f,
		&layout
	);

	const float blur = maxf(0.f, m_snap.TextBlurRadius);
	const float o = (blur <= 0.1f) ? 0.f : blur * 0.25f;

	ComPtr<ID2D1SolidColorBrush> br;
	rt->CreateSolidColorBrush(D2D1::ColorF(col.r, col.g, col.b, col.a), &br);

	D2D1::Matrix3x2F old;
	rt->GetTransform(&old);
	const auto rot = D2D1::Matrix3x2F::Rotation(m_snap.TextAngleDeg, origin);
	rt->SetTransform(rot * old);

	if (o > 0.0f) {
		ComPtr<ID2D1SolidColorBrush> brShadow;
		rt->CreateSolidColorBrush(D2D1::ColorF(col.r, col.g, col.b, col.a * 0.35f), &brShadow);
		rt->DrawTextLayout(D2D1::Point2F(origin.x + o, origin.y + o), layout.Get(), brShadow.Get());
		rt->DrawTextLayout(D2D1::Point2F(origin.x - o, origin.y + o), layout.Get(), brShadow.Get());
		rt->DrawTextLayout(D2D1::Point2F(origin.x + o, origin.y - o), layout.Get(), brShadow.Get());
		rt->DrawTextLayout(D2D1::Point2F(origin.x - o, origin.y - o), layout.Get(), brShadow.Get());
	}

	rt->DrawTextLayout(origin, layout.Get(), br.Get());
	rt->SetTransform(&old);
}

D2D1_SIZE_F RendererText::MeasureUnwrapped(IDWriteFactory* dwFactory, const std::wstring& text)
{
	if (!m_fmt) return D2D1::SizeF(0, 0);

	ComPtr<IDWriteTextLayout> layout;
	dwFactory->CreateTextLayout(
		text.c_str(), (UINT32)text.size(),
		m_fmt.Get(),
		4096.0f, 4096.0f,
		&layout
	);
	DWRITE_TEXT_METRICS m{};
	if (FAILED(layout->GetMetrics(&m))) return D2D1::SizeF(0, 0);
	return D2D1::SizeF(m.width, m.height);
}

D2D1_SIZE_F RendererText::MeasureWrapped(IDWriteFactory* dwFactory, const std::wstring& text, float width)
{
	if (!m_fmt) return D2D1::SizeF(0, 0);

	ComPtr<IDWriteTextLayout> layout;
	dwFactory->CreateTextLayout(
		text.c_str(), (UINT32)text.size(),
		m_fmt.Get(),
		width, 4096.0f,
		&layout
	);
	DWRITE_TEXT_METRICS m{};
	if (FAILED(layout->GetMetrics(&m))) return D2D1::SizeF(0, 0);
	return D2D1::SizeF(m.width, m.height);
}

//===================== macro expansion =====================
std::wstring RendererText::ExpandTextMacros(const std::wstring& fmt, const std::wstring& ts)
{
	std::wstring out = fmt;

	// user / machine
	const std::wstring uname = GetOsUserNameSimple();

	wchar_t comp[MAX_COMPUTERNAME_LENGTH + 2] = L"";
	DWORD cl = (DWORD)(sizeof(comp) / sizeof(comp[0]));
	if (!GetComputerNameW(comp, &cl) || cl == 0) comp[0] = L'\0';
	const std::wstring mname = comp;

	// time basics
	SYSTEMTIME st{}; GetLocalTime(&st);
	wchar_t shortDate[32];  swprintf_s(shortDate, L"%02d/%02d/%04d", st.wDay, st.wMonth, st.wYear);
	wchar_t shortTime[32];  swprintf_s(shortTime, L"%02d:%02d", st.wHour, st.wMinute);
	wchar_t longTime[32];   swprintf_s(longTime, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
	wchar_t longDate[64];   swprintf_s(longDate, L"%02d/%02d/%04d", st.wDay, st.wMonth, st.wYear);

	ReplaceAllCI(out, L"{UserName}", uname);
	ReplaceAllCI(out, L"{username}", uname);
	ReplaceAllCI(out, L"{MachineName}", mname);
	ReplaceAllCI(out, L"{machinename}", mname);
	ReplaceAllCI(out, L"{ShortDate}", shortDate);
	ReplaceAllCI(out, L"{shortdate}", shortDate);
	ReplaceAllCI(out, L"{ShortTime}", shortTime);
	ReplaceAllCI(out, L"{shorttime}", shortTime);
	ReplaceAllCI(out, L"{LongDate}", longDate);
	ReplaceAllCI(out, L"{longdate}", longDate);
	ReplaceAllCI(out, L"{LongTime}", longTime);
	ReplaceAllCI(out, L"{longtime}", longTime);
	ReplaceAllCI(out, L"{CustomDateTime}", ts);
	ReplaceAllCI(out, L"{customdatetime}", ts);
	return out;
}

//===================== utf8 logger =====================
std::string RendererText::wide_to_utf8(const std::wstring& w)
{
	if (w.empty()) return {};
	int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s; s.resize((size_t)len);
	if (len > 0) {
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, nullptr, nullptr);
	}
	return s;
}
