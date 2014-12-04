// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#include "VideoBackends/DX11/BoundingBox.h"
#include "VideoBackends/DX11/D3DBase.h"
#include "VideoBackends/DX11/PixelShaderCache.h"
#include "VideoBackends/DX11/Render.h"
#include "VideoBackends/DX11/VertexManager.h"
#include "VideoBackends/DX11/VertexShaderCache.h"

#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/Debugger.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/MainBase.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"

// internal state for loading vertices
extern NativeVertexFormat *g_nativeVertexFmt;

namespace DX11
{

void VertexManager::CreateDeviceObjects()
{
	D3D11_BUFFER_DESC bufdesc = CD3D11_BUFFER_DESC(MAX_BUFFER_SIZE,
		D3D11_BIND_INDEX_BUFFER | D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
	
	m_vertexDrawOffset = 0;
	m_indexDrawOffset = 0;
	
	for (int i = 0; i < MAX_BUFFER_COUNT; i++)
	{
		m_buffers[i] = nullptr;
		CHECK(SUCCEEDED(D3D::device->CreateBuffer(&bufdesc, nullptr, D3D::ToAddr(m_buffers[i]))), "Failed to create buffer.");
		D3D::SetDebugObjectName((ID3D11DeviceChild*)m_buffers[i].get(), "Buffer of VertexManager");
	}
	
	m_currentBuffer = 0;
	m_bufferCursor = MAX_BUFFER_SIZE;
	
	m_lineAndPointShader.Init();
}

void VertexManager::DestroyDeviceObjects()
{
	m_lineAndPointShader.Shutdown();
	
	for (int i = 0; i < MAX_BUFFER_COUNT; i++)
	{
		m_buffers[i].reset();
	}
}

VertexManager::VertexManager()
{
	LocalVBuffer.resize(MAXVBUFFERSIZE);

	s_pCurBufferPointer = s_pBaseBufferPointer = &LocalVBuffer[0];
	s_pEndBufferPointer = s_pBaseBufferPointer + LocalVBuffer.size();

	LocalIBuffer.resize(MAXIBUFFERSIZE);

	CreateDeviceObjects();
}

VertexManager::~VertexManager()
{
	DestroyDeviceObjects();
}

void VertexManager::PrepareDrawBuffers(u32 stride)
{
	D3D11_MAPPED_SUBRESOURCE map;
	u32 vertexBufferSize = u32(s_pCurBufferPointer - s_pBaseBufferPointer);
	u32 indexBufferSize = IndexGenerator::GetIndexLen() * sizeof(u16);
	u32 totalBufferSize = vertexBufferSize + indexBufferSize;
	u32 cursor = m_bufferCursor;
	u32 padding = m_bufferCursor % stride;
	if (padding)
	{
		cursor += stride - padding;
	}
	D3D11_MAP MapType = D3D11_MAP_WRITE_NO_OVERWRITE;
	if (cursor + totalBufferSize >= MAX_BUFFER_SIZE)
	{
		// Wrap around
		m_currentBuffer = (m_currentBuffer + 1) % MAX_BUFFER_COUNT;
		cursor = 0;
		MapType = D3D11_MAP_WRITE_DISCARD;
	}
	m_vertexDrawOffset = cursor;
	m_indexDrawOffset = cursor + vertexBufferSize;	

	D3D::context->Map(m_buffers[m_currentBuffer].get(), 0, MapType, 0, &map);
	u8* mappedData = reinterpret_cast<u8*>(map.pData);
	memcpy(mappedData + m_vertexDrawOffset, s_pBaseBufferPointer, vertexBufferSize);
	memcpy(mappedData + m_indexDrawOffset, GetIndexBuffer(), indexBufferSize);
	D3D::context->Unmap(m_buffers[m_currentBuffer].get(), 0);

	m_bufferCursor = cursor + totalBufferSize;

	ADDSTAT(stats.thisFrame.bytesVertexStreamed, vertexBufferSize);
	ADDSTAT(stats.thisFrame.bytesIndexStreamed, indexBufferSize);
}

static const float LINE_PT_TEX_OFFSETS[8] = {
	0.f, 0.0625f, 0.125f, 0.25f, 0.5f, 1.f, 1.f, 1.f
};

void VertexManager::Draw(UINT stride)
{
	u32 components = g_nativeVertexFmt->m_components;
	u32 indices = IndexGenerator::GetIndexLen();

	D3D::context->IASetVertexBuffers(0, 1, D3D::ToAddr(m_buffers[m_currentBuffer]), &stride, &m_vertexDrawOffset);
	D3D::context->IASetIndexBuffer(m_buffers[m_currentBuffer].get(), DXGI_FORMAT_R16_UINT, m_indexDrawOffset);
		
	if (current_primitive_type == PRIMITIVE_TRIANGLES)
	{
		auto pt = g_ActiveConfig.backend_info.bSupportsPrimitiveRestart ?
		D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP :
		D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

		D3D::context->IASetPrimitiveTopology(pt);
		D3D::context->GSSetShader(nullptr, nullptr, 0);

		D3D::context->DrawIndexed(indices, 0, 0);
		INCSTAT(stats.thisFrame.numDrawCalls);
	}
	else if (current_primitive_type == PRIMITIVE_LINES)
	{
		float lineWidth = float(bpmem.lineptwidth.linesize) / 6.f;
		float texOffset = LINE_PT_TEX_OFFSETS[bpmem.lineptwidth.lineoff];
		float vpWidth = 2.0f * xfmem.viewport.wd;
		float vpHeight = -2.0f * xfmem.viewport.ht;

		bool texOffsetEnable[8];

		for (int i = 0; i < 8; ++i)
			texOffsetEnable[i] = bpmem.texcoords[i].s.line_offset;

		if (m_lineAndPointShader.SetLineShader(g_nativeVertexFmt->m_components, lineWidth,
			texOffset, vpWidth, vpHeight, texOffsetEnable))
		{
			((DX11::Renderer*)g_renderer)->ApplyCullDisable();
			D3D::context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
			D3D::context->DrawIndexed(indices, 0, 0);
			INCSTAT(stats.thisFrame.numDrawCalls);

			D3D::context->GSSetShader(nullptr, nullptr, 0);
		}
	}
	else
	{
		float pointSize = float(bpmem.lineptwidth.pointsize) / 6.f;
		float texOffset = LINE_PT_TEX_OFFSETS[bpmem.lineptwidth.pointoff];
		float vpWidth = 2.0f * xfmem.viewport.wd;
		float vpHeight = -2.0f * xfmem.viewport.ht;

		bool texOffsetEnable[8];

		for (int i = 0; i < 8; ++i)
			texOffsetEnable[i] = bpmem.texcoords[i].s.point_offset;

		if (m_lineAndPointShader.SetPointShader(g_nativeVertexFmt->m_components, pointSize,
			texOffset, vpWidth, vpHeight, texOffsetEnable))
		{
			D3D::context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
			D3D::context->DrawIndexed(indices, 0, 0);
			INCSTAT(stats.thisFrame.numDrawCalls);

			D3D::context->GSSetShader(nullptr, nullptr, 0);
		}
	}
	
	if (current_primitive_type != PRIMITIVE_TRIANGLES)
		((DX11::Renderer*)g_renderer)->RestoreCull();
}

void VertexManager::PrepareShaders(u32 components, const XFMemory &xfr, const BPMemory &bpm, bool ongputhread)
{
	if (ongputhread)
	{
		if (!s_Shader_Refresh_Required)
		{
			return;
		}
		s_Shader_Refresh_Required = false;
	}
	bool useDstAlpha = !g_ActiveConfig.bDstAlphaPass && bpm.dstalpha.enable && bpm.blendmode.alphaupdate &&
		bpm.zcontrol.pixel_format == PEControl::RGBA6_Z24;
	VertexShaderCache::PrepareShader(components, xfr, bpm, ongputhread);
	PixelShaderCache::PrepareShader(useDstAlpha ? DSTALPHA_DUAL_SOURCE_BLEND : DSTALPHA_NONE, components, xfr, bpm, ongputhread);
}

void VertexManager::vFlush(bool useDstAlpha)
{
	if (!PixelShaderCache::TestShader())
	{
		return;
	}
	if (!VertexShaderCache::TestShader())
	{
		return;
	}
	if (g_ActiveConfig.backend_info.bSupportsBBox && BoundingBox::active)
	{
		ID3D11UnorderedAccessView* uav = BBox::GetUAV();
		D3D::context->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL, nullptr, nullptr, 2, 1, &uav, nullptr);
	}
	unsigned int stride = g_nativeVertexFmt->GetVertexStride();
	PrepareDrawBuffers(stride);
	g_nativeVertexFmt->SetupVertexPointers();
	g_renderer->ApplyState(useDstAlpha);
	
	Draw(stride);

	g_renderer->RestoreState();
}

void VertexManager::ResetBuffer(u32 stride)
{
	s_pCurBufferPointer = s_pBaseBufferPointer;
	IndexGenerator::Start(GetIndexBuffer());
}

}  // namespace
