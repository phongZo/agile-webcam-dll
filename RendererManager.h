#include "pch.h"
#pragma once
#include "RendererBase.h"
#include "RenderSnapshot.h"

#include <d2d1.h>
#include <dwrite.h>
#include <Windows.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>
#include <vector>
#include <rpcndr.h>

class RendererManager
{
public:
	static RendererManager& Instance();

	void Init(ID2D1Factory* d2dFactory, IDWriteFactory* dwFactory);

	boolean SetSnapshot(const RenderSnapshot& snap);

	bool HasSnapshot() const { return m_hasSnapshot; }
	const RenderSnapshot& GetSnapshot() const { return m_snapshot; }

	void DrawOverlay(ID2D1RenderTarget* rt, const SIZE& size);

private:
	RendererManager() = default;

	void ApplySnapshotToRenderers_Locked();

	Microsoft::WRL::ComPtr<ID2D1Factory>   m_d2dFactory;
	Microsoft::WRL::ComPtr<IDWriteFactory> m_dwFactory;

	std::vector<std::unique_ptr<RendererBase>> m_renderers;

	std::mutex       m_mx;
	RenderSnapshot   m_snapshot;
	bool             m_hasSnapshot = false;
	std::size_t      m_lastSig{ 0 };
};
