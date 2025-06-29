// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/VideoBackend.h"

#include <memory>
#include <string>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DBoundingBox.h"
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D/D3DPerfQuery.h"
#include "VideoBackends/D3D/D3DSwapChain.h"
#include "VideoBackends/D3D/D3DVertexManager.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/ShaderCache.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VROpenVR.h"             // For VROpenVR
#include "VideoCommon/VideoConfig.h"          // For g_ActiveConfig and StereoMode
#include "VideoBackends/D3DCommon/D3DCommon.h"  // For D3DCommon::CreateDXGIFactory
#include <dxgi.h>                             // For DXGI_ADAPTER_DESC1, LUID
#include <wrl/client.h>                       // For ComPtr


namespace DX11
{
std::string VideoBackend::GetName() const
{
  return NAME;
}

std::string VideoBackend::GetDisplayName() const
{
  return _trans("Direct3D 11");
}

std::optional<std::string> VideoBackend::GetWarningMessage() const
{
  std::optional<std::string> result;

  // If relevant, show a warning about partial DX11.1 support
  // This is being called BEFORE FillBackendInfo is called for this backend,
  // so query for logic op support manually
  bool supportsLogicOp = false;
  if (D3DCommon::LoadLibraries())
  {
    supportsLogicOp = D3D::SupportsLogicOp(g_Config.iAdapter);
    D3DCommon::UnloadLibraries();
  }

  if (!supportsLogicOp)
  {
    result = _trans("The Direct3D 11 renderer requires support for features not supported by your "
                    "system configuration. You may still use this backend, but you will encounter "
                    "graphical artifacts in certain games.\n"
                    "\n"
                    "Do you really want to switch to Direct3D 11? If unsure, select 'No'.");
  }

  return result;
}

void VideoBackend::InitBackendInfo(const WindowSystemInfo& wsi)
{
  if (!D3DCommon::LoadLibraries())
    return;

  FillBackendInfo();
  D3DCommon::UnloadLibraries();
}

void VideoBackend::FillBackendInfo()
{
  g_backend_info.api_type = APIType::D3D;
  g_backend_info.MaxTextureSize = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  g_backend_info.bUsesLowerLeftOrigin = false;
  g_backend_info.bSupportsExclusiveFullscreen = true;
  g_backend_info.bSupportsDualSourceBlend = true;
  g_backend_info.bSupportsPrimitiveRestart = true;
  g_backend_info.bSupportsGeometryShaders = true;
  g_backend_info.bSupportsComputeShaders = false;
  g_backend_info.bSupports3DVision = true;
  g_backend_info.bSupportsPostProcessing = true;
  g_backend_info.bSupportsPaletteConversion = true;
  g_backend_info.bSupportsClipControl = true;
  g_backend_info.bSupportsDepthClamp = true;
  g_backend_info.bSupportsReversedDepthRange = false;
  g_backend_info.bSupportsMultithreading = false;
  g_backend_info.bSupportsGPUTextureDecoding = true;
  g_backend_info.bSupportsCopyToVram = true;
  g_backend_info.bSupportsLargePoints = false;
  g_backend_info.bSupportsDepthReadback = true;
  g_backend_info.bSupportsPartialDepthCopies = false;
  g_backend_info.bSupportsBitfield = false;
  g_backend_info.bSupportsDynamicSamplerIndexing = false;
  g_backend_info.bSupportsFramebufferFetch = false;
  g_backend_info.bSupportsBackgroundCompiling = true;
  g_backend_info.bSupportsST3CTextures = true;
  g_backend_info.bSupportsBPTCTextures = true;
  g_backend_info.bSupportsEarlyZ = true;
  g_backend_info.bSupportsBBox = true;
  g_backend_info.bSupportsFragmentStoresAndAtomics = true;
  g_backend_info.bSupportsGSInstancing = true;
  g_backend_info.bSupportsSSAA = true;
  g_backend_info.bSupportsShaderBinaries = true;
  g_backend_info.bSupportsPipelineCacheData = false;
  g_backend_info.bSupportsCoarseDerivatives = true;
  g_backend_info.bSupportsTextureQueryLevels = true;
  g_backend_info.bSupportsLodBiasInSampler = true;
  g_backend_info.bSupportsLogicOp = D3D::SupportsLogicOp(g_Config.iAdapter);
  g_backend_info.bSupportsSettingObjectNames = true;
  g_backend_info.bSupportsPartialMultisampleResolve = true;
  g_backend_info.bSupportsDynamicVertexLoader = false;
  g_backend_info.bSupportsHDROutput = true;

  g_backend_info.Adapters = D3DCommon::GetAdapterNames();
  g_backend_info.AAModes = D3D::GetAAModes(g_Config.iAdapter);

  // Override optional features if we are actually booting.
  if (D3D::device)
  {
    g_backend_info.bSupportsST3CTextures = D3D::SupportsTextureFormat(DXGI_FORMAT_BC1_UNORM) &&
                                           D3D::SupportsTextureFormat(DXGI_FORMAT_BC2_UNORM) &&
                                           D3D::SupportsTextureFormat(DXGI_FORMAT_BC3_UNORM);
    g_backend_info.bSupportsBPTCTextures = D3D::SupportsTextureFormat(DXGI_FORMAT_BC7_UNORM);

    // Features only supported with a FL11.0+ device.
    const bool shader_model_5_supported = D3D::feature_level >= D3D_FEATURE_LEVEL_11_0;
    g_backend_info.bSupportsEarlyZ = shader_model_5_supported;
    g_backend_info.bSupportsBBox = shader_model_5_supported;
    g_backend_info.bSupportsFragmentStoresAndAtomics = shader_model_5_supported;
    g_backend_info.bSupportsGSInstancing = shader_model_5_supported;
    g_backend_info.bSupportsSSAA = shader_model_5_supported;
    g_backend_info.bSupportsGPUTextureDecoding = shader_model_5_supported;
  }
}

bool VideoBackend::Initialize(const WindowSystemInfo& wsi)
{
  u32 adapter_to_use = g_Config.iAdapter;

  if (g_ActiveConfig.stereo_mode == StereoMode::OpenVR)
  {
    // Create a temporary, local VROpenVR instance for adapter LUID lookup.
    // This instance is separate from Core::g_vr_openvr_instance and is short-lived.
    std::unique_ptr<VROpenVR> temp_vr_system_for_adapter_lookup = std::make_unique<VROpenVR>();
    bool temp_vr_initialized = false;

    // Initialize as Utility app type for adapter lookup, as it's less demanding.
    if (temp_vr_system_for_adapter_lookup->Init(vr::VRApplication_Utility))
    {
      temp_vr_initialized = true;
      INFO_LOG_FMT(VR, "Temporary VROpenVR instance (Utility type) initialized for adapter LUID lookup.");
    }
    else
    {
      ERROR_LOG_FMT(VR, "Failed to initialize temporary VROpenVR (Utility type) for adapter selection. Using default adapter {}.", adapter_to_use);
    }

    if (temp_vr_initialized)
    {
      long long openvr_luid_ll = temp_vr_system_for_adapter_lookup->GetAdapterLUID();
      if (openvr_luid_ll != 0)
      {
        ComPtr<IDXGIFactory> temp_dxgi_factory_base = D3DCommon::CreateDXGIFactory(false);
        ComPtr<IDXGIFactory1> temp_dxgi_factory1;

        if (temp_dxgi_factory_base && SUCCEEDED(temp_dxgi_factory_base.As(&temp_dxgi_factory1)))
        {
          LUID openvr_luid_struct;
          openvr_luid_struct.LowPart = static_cast<DWORD>(openvr_luid_ll);
          openvr_luid_struct.HighPart = static_cast<LONG>(openvr_luid_ll >> 32);
          INFO_LOG_FMT(VIDEO, "OpenVR recommended LUID: Low={}, High={}. Searching DXGI adapters.",
                       openvr_luid_struct.LowPart, openvr_luid_struct.HighPart);

          UINT i = 0;
          ComPtr<IDXGIAdapter1> current_adapter;
          bool found_match = false;
          while (temp_dxgi_factory1->EnumAdapters1(i, current_adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND)
          {
            DXGI_ADAPTER_DESC1 adapter_desc;
            if (SUCCEEDED(current_adapter->GetDesc1(&adapter_desc)))
            {
                const wchar_t* adapter_name1 = std::wstring(adapter_desc.Description).c_str();
              INFO_LOG_FMT(VIDEO, "Adapter {}: {}, LUID: Low={}, High={}", i, (void*)adapter_name1, adapter_desc.AdapterLuid.LowPart, adapter_desc.AdapterLuid.HighPart);
              if (adapter_desc.AdapterLuid.LowPart == openvr_luid_struct.LowPart &&
                  adapter_desc.AdapterLuid.HighPart == openvr_luid_struct.HighPart)
              {
                const wchar_t* adapter_name2 = std::wstring(adapter_desc.Description).c_str();
                INFO_LOG_FMT(VIDEO, "Found matching OpenVR adapter at index {}: {}", i, (void*)adapter_name2);
                adapter_to_use = i;
                found_match = true;
                break;
              }
            }
            i++;
          }

          if (!found_match)
          {
            WARN_LOG_FMT(VIDEO, "OpenVR LUID (Low: {}, High: {}) not found among DXGI adapters. Using default adapter {}.",
                         openvr_luid_struct.LowPart, openvr_luid_struct.HighPart, g_Config.iAdapter);
          }
        }
        else
        {
          WARN_LOG_FMT(VIDEO, "Failed to create/query DXGIFactory1 for OpenVR adapter lookup. Using default adapter {}.", g_Config.iAdapter);
        }
      }
      else
      {
        WARN_LOG_FMT(VIDEO, "OpenVR did not provide a valid LUID. Using default adapter {}.", g_Config.iAdapter);
      }
    }
    // Shutdown the temporary VR system after LUID lookup.
    // VROpenVR::Shutdown() handles being called on an uninitialized or already shutdown instance.
    if (temp_vr_system_for_adapter_lookup) // Check if it wasn't reset due to Init failure
    {
        temp_vr_system_for_adapter_lookup->Shutdown();
        INFO_LOG_FMT(VR, "Temporary VROpenVR instance for adapter LUID lookup shut down.");
    }
    // temp_vr_system_for_adapter_lookup will go out of scope here and be destroyed.
  }

  if (!D3D::Create(adapter_to_use, g_Config.bEnableValidationLayer))
    return false;

  FillBackendInfo();
  UpdateActiveConfig();

  std::unique_ptr<SwapChain> swap_chain;
  if (wsi.render_surface && !(swap_chain = SwapChain::Create(wsi)))
  {
    PanicAlertFmtT("Failed to create D3D swap chain");
    ShutdownShared();
    D3D::Destroy();
    return false;
  }

  auto gfx = std::make_unique<DX11::Gfx>(std::move(swap_chain), wsi.render_surface_scale);
  auto vertex_manager = std::make_unique<VertexManager>();
  auto perf_query = std::make_unique<PerfQuery>();
  auto bounding_box = std::make_unique<D3DBoundingBox>();

  return InitializeShared(std::move(gfx), std::move(vertex_manager), std::move(perf_query),
                          std::move(bounding_box));
}

void VideoBackend::Shutdown()
{
  ShutdownShared();
  D3D::Destroy();
}
}  // namespace DX11
