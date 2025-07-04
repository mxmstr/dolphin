// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "Common/CommonTypes.h"

struct IDXGIFactory;

enum class AbstractTextureFormat : u32;

namespace D3DCommon
{
enum VideoQuadType
{
  MONO_VIDEO_QUAD,
  // Add STEREO_VIDEO_QUAD or other types as needed later
};

void DrawVideoQuad(
    ID3D11ShaderResourceView* texture_srv,
    const D3D11_RECT& source_rect, // Source rectangle in texture pixels
    float dest_width,              // Destination width in screen pixels
    float dest_height,             // Destination height in screen pixels
    float sx_scale,                // Texture coordinate X scale (typically 1.0 or for sub-regions)
    float sy_scale,                // Texture coordinate Y scale (typically 1.0 or for sub-regions)
    float gamma,                   // Gamma correction value
    int eye,                       // Eye index (0 for left, 1 for right) or slice for texture arrays
    VideoQuadType type             // Shader type to use
);

void ShutdownD3DCommonResources();

// Loading dxgi.dll and d3dcompiler.dll
bool LoadLibraries();
void UnloadLibraries();

// Returns a list of D3D device names.
std::vector<std::string> GetAdapterNames();

// Helper function which creates a DXGI factory.
Microsoft::WRL::ComPtr<IDXGIFactory> CreateDXGIFactory(bool debug_device);

// Globally-accessible D3DCompiler function.
extern pD3DCompile d3d_compile;

// Helpers for texture format conversion.
DXGI_FORMAT GetDXGIFormatForAbstractFormat(AbstractTextureFormat format, bool typeless);
DXGI_FORMAT GetSRVFormatForAbstractFormat(AbstractTextureFormat format);
DXGI_FORMAT GetRTVFormatForAbstractFormat(AbstractTextureFormat format, bool integer);
DXGI_FORMAT GetDSVFormatForAbstractFormat(AbstractTextureFormat format);
AbstractTextureFormat GetAbstractFormatForDXGIFormat(DXGI_FORMAT format);

// This function will assign a name to the given resource.
// The DirectX debug layer will make it easier to identify resources that way,
// e.g. when listing up all resources who have unreleased references.
void SetDebugObjectName(IUnknown* resource, std::string_view name);
}  // namespace D3DCommon
