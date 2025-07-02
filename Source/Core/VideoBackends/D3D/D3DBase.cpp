// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DBase.h"

#include <algorithm>
#include <array>
// START VR MERGE - Added includes
#include <dxgi1_2.h> // For IDXGIOutput1, DXGI_ERROR_NOT_FOUND etc.
#include "VideoCommon/VR.h" // For g_hmd_luid, g_hmd_device_name, etc.
#include "VideoCommon/VideoConfig.h" // For g_Config, EFBColorFormat
//#include "Core/HW/Display.h" // For GetMainHWND
// END VR MERGE - Added includes

#include "Common/CommonTypes.h"
#include "Common/DynamicLibrary.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h" // For Common::StringUtils::ToUTF8
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigManager.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoCommon/FramebufferManager.h"
#include <Core/Config/MainSettings.h>
// VideoCommon/VideoConfig.h already included above for VR

extern LUID* g_hmd_luid;

namespace DX11
{
static Common::DynamicLibrary s_d3d11_library;
namespace D3D
{
ComPtr<IDXGIFactory> dxgi_factory; // Should be IDXGIFactory1 or higher for EnumAdapters1
ComPtr<ID3D11Device> device;
ComPtr<ID3D11Device1> device1;
ComPtr<ID3D11DeviceContext> context;
ComPtr<IDXGISwapChain> swapchain; // Added in .h
D3D_FEATURE_LEVEL feature_level;

static ComPtr<ID3D11Debug> s_debug;

constexpr std::array<D3D_FEATURE_LEVEL, 3> s_supported_feature_levels{
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
};

// START VR MERGE - Typedef for D3D11CreateDeviceAndSwapChain
typedef HRESULT(WINAPI* PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, CONST D3D_FEATURE_LEVEL*, UINT, UINT,
    CONST DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);
// END VR MERGE

bool Create(u32 adapter_index, bool enable_debug_layer)
{
  // START VR MERGE - Get HWND
  HWND hwnd = Core::Display::GetMainHWND();
  if (!hwnd)
  {
    PanicAlertFmtT("Failed to get main window handle for D3D initialization.");
    return false;
  }
  // END VR MERGE

  PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN d3d11_create_device_and_swap_chain_ptr = nullptr;

  if (!s_d3d11_library.Open("d3d11.dll") ||
      !s_d3d11_library.GetSymbol("D3D11CreateDeviceAndSwapChain",
                                 &d3d11_create_device_and_swap_chain_ptr))
  {
    PanicAlertFmtT("Failed to load d3d11.dll or find D3D11CreateDeviceAndSwapChain.");
    s_d3d11_library.Close();
    return false;
  }

  if (!D3DCommon::LoadLibraries()) // Loads D3DCompiler
  {
    s_d3d11_library.Close();
    return false;
  }

  ComPtr<IDXGIFactory1> factory1;
  HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory1.GetAddressOf());
  if (FAILED(hr))
  {
      // Try D3DCommon::CreateDXGIFactory as a fallback path
      // This might return IDXGIFactory, IDXGIFactory1 or IDXGIFactory2+
      dxgi_factory = D3DCommon::CreateDXGIFactory(enable_debug_layer);
      if (!dxgi_factory)
      {
          PanicAlertFmtT("Failed to create DXGI factory via D3DCommon::CreateDXGIFactory.");
          D3DCommon::UnloadLibraries();
          s_d3d11_library.Close();
          return false;
      }
      // Query for IDXGIFactory1 from the potentially higher version factory
      hr = dxgi_factory.As(&factory1);
      if (FAILED(hr)) {
          PanicAlertFmtT("Failed to query IDXGIFactory1 from base DXGI factory. VR adapter selection may fail.");
          // Not returning false here, as non-VR path might still work with base dxgi_factory
      }
  }

  // If factory1 was obtained directly or via query, assign it to the global dxgi_factory
  // for other parts of the code that might use dxgi_factory expecting at least IDXGIFactory1 features.
  if (factory1) {
      dxgi_factory = factory1;
  }


  ComPtr<IDXGIAdapter1> selected_adapter1; // Use IDXGIAdapter1 for EnumOutputs1 if needed
  ComPtr<IDXGIOutput> selected_output;

  if (g_has_hmd)
  {
    const LUID* hmd_luid_ptr = g_hmd_luid;
    if (hmd_luid_ptr)
    {
        LUID hmd_luid = *hmd_luid_ptr;
        ComPtr<IDXGIAdapter1> current_adapter;
        for (UINT i = 0; factory1 && factory1->EnumAdapters1(i, current_adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            current_adapter->GetDesc1(&desc);
            if (memcmp(&desc.AdapterLuid, &hmd_luid, sizeof(LUID)) == 0)
            {
                selected_adapter1 = current_adapter;
                INFO_LOG_FMT(VIDEO, "HMD LUID matched adapter: {}", WStringToUTF8(desc.Description));
                break;
            }
        }
    }

    if (selected_adapter1) {
        const std::string& hmd_device_name_str = g_hmd_device_name;
        if (!hmd_device_name_str.empty()) {
            ComPtr<IDXGIOutput> current_output;
            for (UINT j = 0; selected_adapter1->EnumOutputs(j, current_output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++j)
            {
                DXGI_OUTPUT_DESC out_desc;
                current_output->GetDesc(&out_desc);
                MONITORINFOEXA monitor_info;
                monitor_info.cbSize = sizeof(monitor_info);
                if (GetMonitorInfoA(out_desc.Monitor, &monitor_info) &&
                    hmd_device_name_str == monitor_info.szDevice)
                {
                    selected_output = current_output;
                    INFO_LOG_FMT(VIDEO, "HMD device name matched output: {}", hmd_device_name_str);
                    break;
                }
            }
            if (!selected_output) {
                 WARN_LOG_FMT(VIDEO, "HMD LUID found, but device name '{}' did not match any output. Falling back to first output of HMD adapter.", hmd_device_name_str);
                 selected_adapter1->EnumOutputs(0, selected_output.ReleaseAndGetAddressOf());
            }
        } else {
             WARN_LOG_FMT(VIDEO, "HMD LUID found, but HMD device name is empty. Falling back to first output of HMD adapter.");
             selected_adapter1->EnumOutputs(0, selected_output.ReleaseAndGetAddressOf());
        }
    } else if (g_has_hmd) { // Only warn if VR is active but adapter selection failed
        WARN_LOG_FMT(VIDEO, "VR HMD is present but no matching adapter found via LUID. Falling back to default adapter selection.");
    }
  }

  ComPtr<IDXGIAdapter> generic_selected_adapter; // For D3D11CreateDeviceAndSwapChain
  if (!selected_adapter1)
  {
    hr = dxgi_factory->EnumAdapters(adapter_index, generic_selected_adapter.GetAddressOf());
    if (FAILED(hr))
    {
      WARN_LOG_FMT(VIDEO, "Adapter {} not found, using default adapter (index 0): {}", adapter_index, DX11HRWrap(hr));
      hr = dxgi_factory->EnumAdapters(0, generic_selected_adapter.ReleaseAndGetAddressOf());
      if (FAILED(hr)) {
          PanicAlertFmtT("Failed to enumerate any DXGI adapters: {}", DX11HRWrap(hr));
          // Cleanup before returning
          if (factory1) factory1.Reset(); else dxgi_factory.Reset();
          D3DCommon::UnloadLibraries();
          s_d3d11_library.Close();
          return false;
      }
    }
    selected_adapter1.As(&generic_selected_adapter); // Store in the generic ComPtr
  } else {
    generic_selected_adapter = selected_adapter1; // Use the VR selected adapter
  }

  if (generic_selected_adapter && !selected_output)
  {
      hr = generic_selected_adapter->EnumOutputs(0, selected_output.GetAddressOf());
      if(FAILED(hr)) {
          // This case should ideally not be hit if an adapter was successfully selected.
          WARN_LOG_FMT(VIDEO, "Failed to get any output for the selected adapter: {}. This might be an issue with headless systems or unusual driver states.", DX11HRWrap(hr));
          // Proceeding without a specific output, D3D might pick one.
      }
  }

  DXGI_SWAP_CHAIN_DESC swap_chain_desc = {};
  swap_chain_desc.BufferCount = g_has_hmd ? 1 : 2; // 1 for VR (runtime handles it), 2 for window for smoother non-VR
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.OutputWindow = hwnd;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.SampleDesc.Quality = 0;
  swap_chain_desc.Windowed = !Config::Get(Config::MAIN_FULLSCREEN) || g_Config.bBorderlessFullscreen;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; // Recommended for non-stereo, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL for newer paths

  // Tearing: Generally, VR runtimes prefer VSync off on the companion window to avoid extra latency.
  // D3DSwapChain::IsTearingSupported() would be the place to check this.
  // For now, let's assume if VSync is off, we want tearing.
  bool vsync_enabled = g_Config.bVSync; // Check actual VSync setting
  if (!vsync_enabled) { //&& D3DCommon::IsTearingSupported(dxgi_factory.Get())) {
      swap_chain_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  } else {
      swap_chain_desc.Flags = 0;
  }


  DXGI_MODE_DESC buffer_desc = {};
  if (g_has_hmd)
  {
    buffer_desc.Width = g_hmd_window_width;
    buffer_desc.Height = g_hmd_window_height;
    // Hydra's g_is_direct_mode logic might be relevant here if companion window needs specific sizing in direct mode.
  }
  else
  {
    RECT client_rect;
    GetClientRect(hwnd, &client_rect);
    buffer_desc.Width = client_rect.right - client_rect.left;
    buffer_desc.Height = client_rect.bottom - client_rect.top;
    if (buffer_desc.Width == 0) buffer_desc.Width = 1; // Ensure non-zero
    if (buffer_desc.Height == 0) buffer_desc.Height = 1; // Ensure non-zero
  }
  buffer_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  buffer_desc.RefreshRate.Numerator = 0;
  buffer_desc.RefreshRate.Denominator = 0;
  buffer_desc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
  buffer_desc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
  swap_chain_desc.BufferDesc = buffer_desc;

  UINT create_device_flags = 0;
  if (enable_debug_layer)
  {
    create_device_flags |= D3D11_CREATE_DEVICE_DEBUG;
  }

  hr = d3d11_create_device_and_swap_chain_ptr(
      generic_selected_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, create_device_flags,
      s_supported_feature_levels.data(), static_cast<UINT>(s_supported_feature_levels.size()),
      D3D11_SDK_VERSION, &swap_chain_desc, D3D::swapchain.GetAddressOf(), D3D::device.GetAddressOf(),
      &D3D::feature_level, D3D::context.GetAddressOf());

  if (FAILED(hr) && (create_device_flags & D3D11_CREATE_DEVICE_DEBUG))
  {
    WARN_LOG_FMT(VIDEO, "Failed to create D3D11 device and swapchain with debug layer ({}), retrying without.", DX11HRWrap(hr));
    create_device_flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    hr = d3d11_create_device_and_swap_chain_ptr(
        generic_selected_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, create_device_flags,
        s_supported_feature_levels.data(), static_cast<UINT>(s_supported_feature_levels.size()),
        D3D11_SDK_VERSION, &swap_chain_desc, D3D::swapchain.GetAddressOf(), D3D::device.GetAddressOf(),
        &D3D::feature_level, D3D::context.GetAddressOf());
  }

  if (FAILED(hr))
  {
    PanicAlertFmtT(
        "Failed to initialize Direct3D device and swapchain.\nMake sure your video card supports at least D3D 10.0.\nError: {0}",
        DX11HRWrap(hr));
    if (factory1) factory1.Reset(); else dxgi_factory.Reset();
    D3DCommon::UnloadLibraries();
    s_d3d11_library.Close();
    return false;
  }

  if (create_device_flags & D3D11_CREATE_DEVICE_DEBUG) {
      if (SUCCEEDED(D3D::device.As(&s_debug))) {
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
      } else {
          // This means D3D11_CREATE_DEVICE_DEBUG was set, device was created, but As(&s_debug) failed.
          // This could happen if the debug layer isn't correctly installed.
          WARN_LOG_FMT(VIDEO, "Debug layer requested and device created, but failed to query ID3D11Debug interface: {}", DX11HRWrap(hr));
      }
  }

  hr = D3D::device.As(&D3D::device1);
  if (FAILED(hr))
  {
    WARN_LOG_FMT(VIDEO,
                 "Missing Direct3D 11.1 support. Logical operations will not be supported. Error: {}",
                 DX11HRWrap(hr));
  }

  if (dxgi_factory) { // dxgi_factory should be valid here
      hr = dxgi_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
      if (FAILED(hr)) {
          WARN_LOG_FMT(VIDEO, "Failed to set MakeWindowAssociation(DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER): {}", DX11HRWrap(hr));
      }
  }

  stateman = std::make_unique<StateManager>();
  return true;
}

void Destroy()
{
  stateman.reset();

  if (D3D::swapchain)
  {
      D3D::swapchain->SetFullscreenState(FALSE, nullptr);
      D3D::swapchain.Reset();
  }

  if (D3D::context)
  {
    D3D::context->ClearState();
    D3D::context->Flush();
    D3D::context.Reset();
  }

  D3D::device1.Reset();

  // Hold a local ComPtr to s_debug to manage its lifetime carefully during reporting
  ComPtr<ID3D11Debug> local_s_debug = s_debug;
  s_debug.Reset(); // Clear global s_debug early

  ULONG remaining_references_after_device_reset = 0;
  if (D3D::device) { // Check if device was created
    remaining_references_after_device_reset = D3D::device.Reset();
  }


  if (local_s_debug)
  {
    // If device.Reset() returned more than 1, it means something else besides local_s_debug
    // (which was s_debug) was holding a reference.
    if (remaining_references_after_device_reset > 1)
    {
      ERROR_LOG_FMT(VIDEO, "D3D device has {} references before s_debug->ReportLiveDeviceObjects(). Expected 1 (from s_debug itself).", remaining_references_after_device_reset);
      local_s_debug->ReportLiveDeviceObjects(D3D11_RLDO_SUMMARY | D3D11_RLDO_DETAIL);
    } else if (remaining_references_after_device_reset == 1) {
      // This is the expected case if everything is clean: D3D::device was held by s_debug only.
      // No need to call ReportLiveDeviceObjects as it would report s_debug itself.
       NOTICE_LOG_FMT(VIDEO, "D3D device references appear clean before s_debug release.");
    }
    // else remaining_references_after_device_reset == 0, meaning device was already gone or never assigned to s_debug's tracking.

    local_s_debug.Reset(); // Release the debug interface itself
  } else {
    // No debug layer, check remaining_references_after_device_reset directly
    if (remaining_references_after_device_reset > 0) {
        ERROR_LOG_FMT(VIDEO, "Unreleased D3D device references: {}. No debug layer was active.", remaining_references_after_device_reset);
    } else {
        NOTICE_LOG_FMT(VIDEO, "Successfully released all D3D device references. No debug layer was active.");
    }
  }

  if (dxgi_factory) dxgi_factory.Reset();
  D3DCommon::UnloadLibraries();
  s_d3d11_library.Close();
}

std::vector<u32> GetAAModes(u32 adapter_index)
{
  Common::DynamicLibrary temp_lib;
  ComPtr<ID3D11Device> temp_device = D3D::device;
  D3D_FEATURE_LEVEL temp_feature_level = D3D::feature_level;

  if (!temp_device)
  {
    ComPtr<IDXGIFactory1> temp_dxgi_factory;
    HRESULT hr_factory = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)temp_dxgi_factory.GetAddressOf());
    if (FAILED(hr_factory)) {
        ComPtr<IDXGIFactory> base_factory = D3DCommon::CreateDXGIFactory(false);
        if (!base_factory) return {1}; // Return 1x (no MSAA) on factory creation failure
        hr_factory = base_factory.As(&temp_dxgi_factory);
        if(FAILED(hr_factory)) return {1};
    }

    ComPtr<IDXGIAdapter1> adapter;
    temp_dxgi_factory->EnumAdapters1(adapter_index, adapter.GetAddressOf());

    PFN_D3D11_CREATE_DEVICE d3d11_create_device_ptr;
    if (!temp_lib.Open("d3d11.dll") ||
        !temp_lib.GetSymbol("D3D11CreateDevice", &d3d11_create_device_ptr))
    {
      return {1};
    }

    HRESULT hr = d3d11_create_device_ptr(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, s_supported_feature_levels.data(),
        static_cast<UINT>(s_supported_feature_levels.size()), D3D11_SDK_VERSION,
        temp_device.GetAddressOf(), &temp_feature_level, nullptr);
    if (FAILED(hr))
      return {1};
  }

  if (temp_feature_level < D3D_FEATURE_LEVEL_10_1)
    return {1};

  const DXGI_FORMAT target_format = D3DCommon::GetDXGIFormatForAbstractFormat(g_Config.GetEFBColorFormat(), false);
  std::vector<u32> aa_modes;
  aa_modes.push_back(1);
  for (u32 samples = 2; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; samples *= 2)
  {
    UINT quality_levels = 0;
    if (temp_device && SUCCEEDED( // Check temp_device again as it might have failed creation above
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
  if (!D3D::device) return false;
  UINT support;
  if (FAILED(D3D::device->CheckFormatSupport(format, &support)))
    return false;

  return (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0;
}

bool SupportsLogicOp(u32 adapter_index)
{
  Common::DynamicLibrary temp_lib;
  ComPtr<ID3D11Device1> temp_device1 = D3D::device1;
  if (!D3D::device)
  {
    ComPtr<ID3D11Device> temp_device_base;

    ComPtr<IDXGIFactory1> temp_dxgi_factory;
     HRESULT hr_factory = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)temp_dxgi_factory.GetAddressOf());
    if (FAILED(hr_factory)) {
        ComPtr<IDXGIFactory> base_factory = D3DCommon::CreateDXGIFactory(false);
        if (!base_factory) return false;
        hr_factory = base_factory.As(&temp_dxgi_factory);
        if(FAILED(hr_factory)) return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    temp_dxgi_factory->EnumAdapters1(adapter_index, adapter.GetAddressOf());

    PFN_D3D11_CREATE_DEVICE d3d11_create_device_ptr;
    if (!temp_lib.Open("d3d11.dll") ||
        !temp_lib.GetSymbol("D3D11CreateDevice", &d3d11_create_device_ptr))
    {
      return false;
    }

    HRESULT hr = d3d11_create_device_ptr(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, s_supported_feature_levels.data(),
        static_cast<UINT>(s_supported_feature_levels.size()), D3D11_SDK_VERSION,
        temp_device_base.GetAddressOf(), nullptr, nullptr);
    if (FAILED(hr))
      return false;

    if (FAILED(temp_device_base.As(&temp_device1)))
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
