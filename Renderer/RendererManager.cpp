#include "pch.h"
#include "DebugLog.h"
#include "RendererManager.h"
#include "RendererText.h"

#include "RenderSnapshot.h"
#include <cstdio>
#include <d2d1.h>
#include <dcommon.h>
#include <dwrite.h>
#include <memory>
#include <mutex>
#include <rpcndr.h>
#include <Windows.h>
#include <wrl/client.h>

RendererManager& RendererManager::Instance()
{
	static RendererManager g;
	return g;
}

void RendererManager::Init(ID2D1Factory* d2dFactory, IDWriteFactory* dwFactory)
{
	m_d2dFactory = d2dFactory;
	m_dwFactory = dwFactory;

	std::lock_guard<std::mutex> lk(m_mx);

	m_renderers.clear();
	m_renderers.emplace_back(std::make_unique<RendererText>());

	char buf[128];
	sprintf_s(buf, "[DComp][RM] Init: created %zu renderer(s)", m_renderers.size());
	DebugLog::log(buf);

	if (m_hasSnapshot) {
		DebugLog::log("[DComp][RM] Init: snapshot already present -> applying to new renderers");
		ApplySnapshotToRenderers_Locked();
	}
}

void RendererManager::ApplySnapshotToRenderers_Locked()
{
	// Caller MUST hold m_mx
	char buf[128];
	sprintf_s(buf, "[DComp][RM] ApplySnapshotToRenderers: count=%zu", m_renderers.size());
	DebugLog::log(buf);

	for (auto& r : m_renderers) {
		r->ApplySettings(m_snapshot);
	}
}

boolean RendererManager::SetSnapshot(const RenderSnapshot& snap)
{
	const std::size_t sig = snap.Signature();
	{
		std::lock_guard<std::mutex> lk(m_mx);

		if (m_hasSnapshot && sig == m_lastSig) {
			//DebugLog::log("[DComp][SNAP] SetSnapshot: unchanged -> skip");
			return false;
		}

		m_snapshot = snap;
		m_hasSnapshot = true;
		m_lastSig = sig;

		DebugLog::log("[DComp][SNAP] SetSnapshot: applying to renderers...");
		ApplySnapshotToRenderers_Locked();
	}

	char buf[256];
	sprintf_s(buf,
		"[DComp][SNAP] Applied: draw=%d text=%d textOpa=%.2f globOpa=%.2f fmtLen=%zu",
		(int)snap.DrawingEnabled, (int)snap.TextEnabled,
		snap.TextOpacity, snap.Opacity, (size_t)snap.TextFormat.size());
	DebugLog::log(buf);
	return true;
}

void RendererManager::DrawOverlay(ID2D1RenderTarget* rt, const SIZE& size)
{
	if (!rt) {
		DebugLog::log("[DComp][RM] DrawOverlay: null render target -> skip");
		return;
	}

	Microsoft::WRL::ComPtr<IDWriteFactory> dw;
	{
		std::lock_guard<std::mutex> lk(m_mx);
		if (!m_hasSnapshot) {
			DebugLog::log("[DComp][DRAW] no snapshot yet -> skip");
			return;
		}
		dw = m_dwFactory; // copy ComPtr
	}

	if (!dw) {
		DebugLog::log("[DComp][RM] DrawOverlay: DW factory null -> skip");
		return;
	}

	D2D1_SIZE_F sz{ (FLOAT)size.cx, (FLOAT)size.cy };

	for (auto& r : m_renderers)
		r->DrawOverlay(rt, dw.Get(), sz);
}
