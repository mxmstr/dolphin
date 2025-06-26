// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DSwapChain.h"

#include "Common/Assert.h"
#include "Common/Logging/Log.h" // For logging
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoCommon/VideoBackendBase.h" // For g_video_backend
#include "VideoCommon/VROpenVR.h"       // For VROpenVR class
#include "VideoCommon/VideoConfig.h"    // For g_ActiveConfig

namespace DX11
{
SwapChain::SwapChain(const WindowSystemInfo& wsi, IDXGIFactory* dxgi_factory,
                     ID3D11Device* d3d_device)
    : D3DCommon::SwapChain(wsi, dxgi_factory, d3d_device)
{
}

SwapChain::~SwapChain() = default;

std::unique_ptr<SwapChain> SwapChain::Create(const WindowSystemInfo& wsi)
{
  std::unique_ptr<SwapChain> swap_chain =
      std::make_unique<SwapChain>(wsi, D3D::dxgi_factory.Get(), D3D::device.Get());

  // Potentially override dimensions for VR before CreateSwapChain is called.
  // This assumes D3DCommon::SwapChain will use m_width and m_height internally,
  // or that its CreateSwapChain method can be modified to accept override dimensions.
  if (g_ActiveConfig.bEnableStereo) // Placeholder for a specific VR config
  {
    if (g_video_backend && g_video_backend->GetVROpenVR() && g_video_backend->GetVROpenVR()->IsInitialized())
    {
      uint32_t rec_width = 0;
      uint32_t rec_height = 0;
      if (g_video_backend->GetVROpenVR()->GetRecommendedRenderTargetSize(&rec_width, &rec_height))
      {
        if (rec_width > 0 && rec_height > 0)
        {
          // For side-by-side stereo, width is doubled.
          // The D3DCommon::SwapChain needs to be aware of these overridden dimensions.
          // We are directly setting protected members m_width, m_height from base class.
          // This is not ideal but a temporary measure. A better solution would be
          // to pass these as parameters to CreateSwapChain or have a SetDimensions method.
          swap_chain->m_width = rec_width * 2;
          swap_chain->m_height = rec_height;
          GENERIC_LOG_FMT(Common::Log::LogType::VR, Common::Log::LogLevel::LINFO,
                           "Overriding SwapChain dimensions for VR: %u x %u",
                           swap_chain->m_width, swap_chain->m_height);
        }
      }
    }
  }

  if (!swap_chain->CreateSwapChain(WantsStereo(), WantsHDR()))
    return nullptr;

  return swap_chain;
}

bool SwapChain::CreateSwapChainBuffers()
{
  ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&texture));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to get swap chain buffer: {}", DX11HRWrap(hr));
  if (FAILED(hr))
    return false;

  m_texture = DXTexture::CreateAdopted(std::move(texture));
  if (!m_texture)
    return false;

  m_framebuffer = DXFramebuffer::Create(m_texture.get(), nullptr, {});
  if (!m_framebuffer)
    return false;

  return true;
}

void SwapChain::DestroySwapChainBuffers()
{
  m_framebuffer.reset();
  m_texture.reset();
}
}  // namespace DX11
