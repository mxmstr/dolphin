// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DBase.h"

#include <algorithm>
#include <array>

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h" // Required for CharArrayToString
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR.h" // For VR variables

// Forward declare HMD LUID if not available through VR.h (adjust as necessary)
#if !defined(G_HMD_LUID) && !defined(g_hmd_luid)
// This is a temporary shim. Ideally, VR.h provides these.
static LUID* g_hmd_luid = nullptr;
//static char g_hmd_device_name[256] = "";
bool g_has_hmd = false;
int g_hmd_window_width = 0;
int g_hmd_window_height = 0;
bool g_is_direct_mode = false; // Assuming default
#endif


namespace DX11
{
static Common::DynamicLibrary s_d3d11_library;

// Function pointer type for D3D11CreateDeviceAndSwapChain
typedef HRESULT(WINAPI* PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, CONST D3D_FEATURE_LEVEL*, UINT, UINT,
    CONST DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);

namespace D3D
{
ComPtr<IDXGIFactory> dxgi_factory;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11Device1> device1;
ComPtr<ID3D11DeviceContext> context;
D3D_FEATURE_LEVEL feature_level;
ComPtr<IDXGISwapChain1> vr_swapchain; // Swapchain created if VR is active using CreateDeviceAndSwapChain

static ComPtr<ID3D11Debug> s_debug;

constexpr std::array<D3D_FEATURE_LEVEL, 3> s_supported_feature_levels{
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
};

bool Create(const WindowSystemInfo& wsi, u32 adapter_index, bool enable_debug_layer)
{
  PFN_D3D11_CREATE_DEVICE pfnD3D11CreateDevice;
  PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN pfnD3D11CreateDeviceAndSwapChain;

  if (!s_d3d11_library.Open("d3d11.dll") ||
      !s_d3d11_library.GetSymbol("D3D11CreateDevice", &pfnD3D11CreateDevice))
  {
    PanicAlertFmtT("Failed to load d3d11.dll or find D3D11CreateDevice");
    s_d3d11_library.Close();
    return false;
  }

  // Try to get D3D11CreateDeviceAndSwapChain, might not be fatal if it fails and VR not used.
  s_d3d11_library.GetSymbol("D3D11CreateDeviceAndSwapChain", &pfnD3D11CreateDeviceAndSwapChain);

  if (!D3DCommon::LoadLibraries())
  {
    s_d3d11_library.Close();
    return false;
  }

  // TODO: VR-Hydra used PCreateDXGIFactory1 if g_vr_needs_DXGIFactory1.
  // D3DCommon::CreateDXGIFactory might need adjustment or checking if it handles this.
  // For now, assume D3DCommon::CreateDXGIFactory is sufficient.
  dxgi_factory = D3DCommon::CreateDXGIFactory(enable_debug_layer);
  if (!dxgi_factory)
  {
    PanicAlertFmtT("Failed to create DXGI factory");
    D3DCommon::UnloadLibraries();
    s_d3d11_library.Close();
    return false;
  }

  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIOutput> output; // Used for VR HMD output selection
  HRESULT hr = E_FAIL;

  // VR HMD Adapter and Output Selection Logic (from VR-Hydra)
  if (g_hmd_luid) // Check if HMD LUID is provided
  {
    ComPtr<IDXGIAdapter> temp_adapter;
    for (UINT i = 0; dxgi_factory->EnumAdapters(i, temp_adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
      DXGI_ADAPTER_DESC desc;
      temp_adapter->GetDesc(&desc);
      if (memcmp(&desc.AdapterLuid, g_hmd_luid, sizeof(LUID)) == 0)
      {
        adapter = temp_adapter;
        // Try to get the first output of the HMD adapter
        // TODO: Make this configurable as in VR-Hydra if needed
        if (FAILED(adapter->EnumOutputs(0, output.ReleaseAndGetAddressOf())))
        {
            WARN_LOG_FMT(VIDEO, "Failed to enumerate outputs for HMD LUID matched adapter.");
            // Fallback or error handling needed if specific output is critical
        }
        break;
      }
    }
     if (!adapter) {
        WARN_LOG_FMT(VIDEO, "HMD LUID provided but no matching adapter found.");
     }
  }

  if (!adapter && g_hmd_device_name && strlen(g_hmd_device_name) > 0) // Check if HMD device name is provided
  {
    ComPtr<IDXGIAdapter> temp_adapter;
    for (UINT iAdapter = 0; dxgi_factory->EnumAdapters(iAdapter, temp_adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++iAdapter)
    {
      DXGI_ADAPTER_DESC adapter_desc;
      temp_adapter->GetDesc(&adapter_desc);
      ComPtr<IDXGIOutput> temp_output;
      for (UINT iOutput = 0; temp_adapter->EnumOutputs(iOutput, temp_output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++iOutput)
      {
        DXGI_OUTPUT_DESC output_desc;
        temp_output->GetDesc(&output_desc);
        MONITORINFOEXA monitor_info;
        monitor_info.cbSize = sizeof(monitor_info);
        if (::GetMonitorInfoA(output_desc.Monitor, &monitor_info) &&
            strcmp(monitor_info.szDevice, g_hmd_device_name) == 0)
        {
          adapter = temp_adapter;
          output = temp_output;
          break;
        }
      }
      if (adapter) break;
    }
    if (!adapter) {
        WARN_LOG_FMT(VIDEO, "HMD Device Name provided but no matching adapter/output found.");
    }
  }

  // Fallback to specified adapter_index or default if VR selection failed or not applicable
  if (!adapter)
  {
    hr = dxgi_factory->EnumAdapters(adapter_index, adapter.GetAddressOf());
    if (FAILED(hr))
    {
      WARN_LOG_FMT(VIDEO, "Adapter {} not found, using default adapter: {}", adapter_index, DX11HRWrap(hr));
      hr = dxgi_factory->EnumAdapters(0, adapter.GetAddressOf()); // Default adapter
      if(FAILED(hr)){
        PanicAlertFmtT("Failed to get default adapter: {}", DX11HRWrap(hr));
        dxgi_factory.Reset();
        D3DCommon::UnloadLibraries();
        s_d3d11_library.Close();
        return false;
      }
    }
    // Get default output for non-VR or fallback
    if (adapter && FAILED(adapter->EnumOutputs(0, output.ReleaseAndGetAddressOf())))
    {
        WARN_LOG_FMT(VIDEO, "Failed to get default output for selected adapter.");
        // Output may not be strictly necessary for D3D11CreateDevice, but is for CreateDeviceAndSwapChain
    }
  }


  // Device and SwapChain Creation
  if (g_has_hmd && pfnD3D11CreateDeviceAndSwapChain)
  {
    // VR Path: Use D3D11CreateDeviceAndSwapChain
    DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
    swap_chain_desc.BufferCount = 1; // Typically 1 for VR, SDK handles frame buffering
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.OutputWindow = (HWND)wsi.render_surface;
    swap_chain_desc.SampleDesc.Count = 1; // No MSAA on the swapchain itself for VR
    swap_chain_desc.SampleDesc.Quality = 0;
    swap_chain_desc.Windowed = TRUE; // VR typically uses windowed mode for direct mode
                                     // SConfig::GetInstance().bFullscreen || Config::Get(GFX_BORDERLESS_FULLSCREEN);
                                     // VR Hydra logic: !SConfig::GetInstance().bFullscreen || g_ActiveConfig.bBorderlessFullscreen;
                                     // Forcing windowed for VR seems common.

    // VR Hydra: swap_chain_desc.Flags = allow_tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    // Tearing is less of an issue for HMDs due to their direct update mechanisms.
    // OpenVR might prefer specific flags or no flags. Start with 0.
    swap_chain_desc.Flags = 0;


    DXGI_MODE_DESC mode_desc = {};
    mode_desc.Width = static_cast<UINT>(g_hmd_window_width);
    mode_desc.Height = static_cast<UINT>(g_hmd_window_height);
    mode_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // Common format for VR
    mode_desc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    // VR-Hydra: hr = output->FindClosestMatchingMode(&mode_desc, &swap_chain_desc.BufferDesc, nullptr);
    // For direct mode, often the HMD dimensions are set directly.
    swap_chain_desc.BufferDesc = mode_desc;


    if (g_is_direct_mode) // Ensure buffer matches HMD res in direct mode
    {
      swap_chain_desc.BufferDesc.Width = static_cast<UINT>(g_hmd_window_width);
      swap_chain_desc.BufferDesc.Height = static_cast<UINT>(g_hmd_window_height);
    }
    else // If not direct mode, use window client rect (though this path less common for VR)
    {
       RECT client_rect;
       GetClientRect((HWND)wsi.render_surface, &client_rect);
       swap_chain_desc.BufferDesc.Width = client_rect.right - client_rect.left;
       swap_chain_desc.BufferDesc.Height = client_rect.bottom - client_rect.top;
    }


    UINT create_device_flags = 0;
    if (enable_debug_layer)
      create_device_flags |= D3D11_CREATE_DEVICE_DEBUG;

    ComPtr<IDXGISwapChain> temp_swapchain;
    hr = pfnD3D11CreateDeviceAndSwapChain(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, create_device_flags,
        s_supported_feature_levels.data(), static_cast<UINT>(s_supported_feature_levels.size()),
        D3D11_SDK_VERSION, &swap_chain_desc, temp_swapchain.GetAddressOf(),
        device.GetAddressOf(), &feature_level, context.GetAddressOf());

    if (FAILED(hr) && enable_debug_layer) // Try without debug layer if debug failed
    {
      WARN_LOG_FMT(VIDEO, "D3D11CreateDeviceAndSwapChain with debug layer failed ({}). Retrying without.", DX11HRWrap(hr));
      create_device_flags &= ~D3D11_CREATE_DEVICE_DEBUG;
      hr = pfnD3D11CreateDeviceAndSwapChain(
          adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, create_device_flags,
          s_supported_feature_levels.data(), static_cast<UINT>(s_supported_feature_levels.size()),
          D3D11_SDK_VERSION, &swap_chain_desc, temp_swapchain.GetAddressOf(),
          device.GetAddressOf(), &feature_level, context.GetAddressOf());
    }
    if (SUCCEEDED(hr))
    {
        hr = temp_swapchain.As(&vr_swapchain); // Store as IDXGISwapChain1
        if (FAILED(hr)) {
            WARN_LOG_FMT(VIDEO, "Failed to query IDXGISwapChain1 from VR swapchain: {}", DX11HRWrap(hr));
            // vr_swapchain will be null, downstream code (like SwapChain class) should handle this.
        }
    }
  }
  else
  {
    // Non-VR Path or D3D11CreateDeviceAndSwapChain not found: Use D3D11CreateDevice
    UINT create_device_flags = 0;
    if (enable_debug_layer)
      create_device_flags |= D3D11_CREATE_DEVICE_DEBUG;

    hr = pfnD3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, create_device_flags,
        s_supported_feature_levels.data(), static_cast<UINT>(s_supported_feature_levels.size()),
        D3D11_SDK_VERSION, device.GetAddressOf(), &feature_level, context.GetAddressOf());

    if (FAILED(hr) && enable_debug_layer) // Try without debug layer if debug failed
    {
      WARN_LOG_FMT(VIDEO, "D3D11CreateDevice with debug layer failed ({}). Retrying without.", DX11HRWrap(hr));
      create_device_flags &= ~D3D11_CREATE_DEVICE_DEBUG;
      hr = pfnD3D11CreateDevice(
          adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, create_device_flags,
          s_supported_feature_levels.data(), static_cast<UINT>(s_supported_feature_levels.size()),
          D3D11_SDK_VERSION, device.GetAddressOf(), &feature_level, context.GetAddressOf());
    }
  }

  if (FAILED(hr))
  {
    PanicAlertFmtT("Failed to create Direct3D device (VR path: {}, Non-VR path: {}).\nMake sure "
                   "your video card supports at least D3D 10.0\n{}",
                   (g_has_hmd && pfnD3D11CreateDeviceAndSwapChain),
                   !(g_has_hmd && pfnD3D11CreateDeviceAndSwapChain), DX11HRWrap(hr));
    dxgi_factory.Reset();
    D3DCommon::UnloadLibraries();
    s_d3d11_library.Close();
    return false;
  }

  // Common setup after device creation (debug layer, device1, stateman)
  if (enable_debug_layer && SUCCEEDED(device.As(&s_debug)))
  {
    ComPtr<ID3D11InfoQueue> info_queue;
    if (SUCCEEDED(s_debug.As(&info_queue)))
    {
      info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, true);
      info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, true);
      D3D11_MESSAGE_ID hide[] = {D3D11_MESSAGE_ID_SETPRIVATEDATA_CHANGINGPARAMS};
      D3D11_INFO_QUEUE_FILTER filter = {};
      filter.DenyList.NumIDs = sizeof(hide) / sizeof(D3D11_MESSAGE_ID);
      filter.DenyList.pIDList = hide;
      info_queue->AddStorageFilterEntries(&filter);
    }
  }
  else if (enable_debug_layer && !s_debug) // If debug was requested but s_debug is null
  {
      WARN_LOG_FMT(VIDEO, "Debug layer requested but failed to get ID3D11Debug interface.");
  }


  hr = device.As(&device1);
  if (FAILED(hr))
  {
    WARN_LOG_FMT(VIDEO,
                 "Missing Direct3D 11.1 support. Logical operations will not be supported.\n{}",
                 DX11HRWrap(hr));
  }

  // Prevent DXGI from responding to Alt+Enter (from VR-Hydra)
  // This might be better placed in SwapChain creation or main window setup
  if (wsi.render_surface)
  {
    ComPtr<IDXGIFactory1> factory1;
    if (SUCCEEDED(dxgi_factory.As(&factory1))) // MakeWindowAssociation is on IDXGIFactory1+
    {
        hr = factory1->MakeWindowAssociation((HWND)wsi.render_surface, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(hr))
            WARN_LOG_FMT(VIDEO, "Failed to set MakeWindowAssociation: {}", DX11HRWrap(hr));
    }
  }


  stateman = std::make_unique<StateManager>();
  return true;
}

void Destroy()
{
  stateman.reset();

  if (context) // Ensure context exists before ClearState/Flush
  {
    context->ClearState();
    context->Flush();
  }

  vr_swapchain.Reset(); // Release VR swapchain if it was created

  context.Reset();
  device1.Reset();

  ULONG remaining_references = 0;
  if (device) // Check if device exists before trying to Reset and get ref count
    remaining_references = device.Reset();

  if (s_debug)
  {
    if (remaining_references > 0) // s_debug adds a ref, so only decrement if there were other refs
        --remaining_references;

    if (remaining_references > 0) // Check after potential decrement
    {
      s_debug->ReportLiveDeviceObjects(D3D11_RLDO_SUMMARY | D3D11_RLDO_DETAIL);
    }
    s_debug.Reset();
  }

  if (remaining_references > 0) // Check final ref count
    ERROR_LOG_FMT(VIDEO, "Unreleased D3D device references: {}.", remaining_references);
  else
    NOTICE_LOG_FMT(VIDEO, "Successfully released all D3D device references!");

  dxgi_factory.Reset();
  D3DCommon::UnloadLibraries();
  s_d3d11_library.Close();
}

std::vector<u32> GetAAModes(u32 adapter_index)
{
  // Use temporary device if we don't have one already.
  Common::DynamicLibrary temp_lib;
  ComPtr<ID3D11Device> temp_device = device;
  D3D_FEATURE_LEVEL temp_feature_level = feature_level;
  if (!temp_device)
  {
    ComPtr<IDXGIFactory> temp_dxgi_factory = D3DCommon::CreateDXGIFactory(false);
    if (!temp_dxgi_factory)
      return {};

    ComPtr<IDXGIAdapter> adapter;
    temp_dxgi_factory->EnumAdapters(adapter_index, adapter.GetAddressOf());

    PFN_D3D11_CREATE_DEVICE d3d11_create_device;
    if (!temp_lib.Open("d3d11.dll") ||
        !temp_lib.GetSymbol("D3D11CreateDevice", &d3d11_create_device))
    {
      return {};
    }

    HRESULT hr = d3d11_create_device(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, s_supported_feature_levels.data(),
        static_cast<UINT>(s_supported_feature_levels.size()), D3D11_SDK_VERSION,
        temp_device.GetAddressOf(), &temp_feature_level, nullptr);
    if (FAILED(hr))
      return {};
  }

  // NOTE: D3D 10.0 doesn't support multisampled resources which are bound as depth buffers AND
  // shader resources. Thus, we can't have MSAA with 10.0 level hardware.
  if (temp_feature_level == D3D_FEATURE_LEVEL_10_0)
    return {};

  const DXGI_FORMAT target_format =
      D3DCommon::GetDXGIFormatForAbstractFormat(FramebufferManager::GetEFBColorFormat(), false);
  std::vector<u32> aa_modes;
  for (u32 samples = 1; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; ++samples)
  {
    UINT quality_levels = 0;
    if (SUCCEEDED(
            temp_device->CheckMultisampleQualityLevels(target_format, samples, &quality_levels)) &&
        quality_levels > 0)
    {
      aa_modes.push_back(samples);
    }
  }

  return aa_modes;
}

bool SupportsTextureFormat(DXGI_FORMAT format)
{
  UINT support;
  if (FAILED(device->CheckFormatSupport(format, &support)))
    return false;

  return (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
}

bool SupportsLogicOp(u32 adapter_index)
{
  // Use temporary device if we don't have one already.
  Common::DynamicLibrary temp_lib;
  ComPtr<ID3D11Device1> temp_device1 = device1;
  if (!device)
  {
    ComPtr<ID3D11Device> temp_device;

    ComPtr<IDXGIFactory> temp_dxgi_factory = D3DCommon::CreateDXGIFactory(false);
    if (!temp_dxgi_factory)
      return false;

    ComPtr<IDXGIAdapter> adapter;
    temp_dxgi_factory->EnumAdapters(adapter_index, adapter.GetAddressOf());

    PFN_D3D11_CREATE_DEVICE d3d11_create_device;
    if (!temp_lib.Open("d3d11.dll") ||
        !temp_lib.GetSymbol("D3D11CreateDevice", &d3d11_create_device))
    {
      return false;
    }

    HRESULT hr = d3d11_create_device(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, s_supported_feature_levels.data(),
        static_cast<UINT>(s_supported_feature_levels.size()), D3D11_SDK_VERSION,
        temp_device.GetAddressOf(), nullptr, nullptr);
    if (FAILED(hr))
      return false;

    if (FAILED(temp_device.As(&temp_device1)))
      return false;
  }

  if (!temp_device1)
    return false;

  D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
  if (FAILED(temp_device1->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options,
                                               sizeof(options))))
  {
    return false;
  }

  return options.OutputMergerLogicOp != FALSE;
}

}  // namespace D3D

}  // namespace DX11
