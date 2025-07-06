// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DSwapChain.h"

#include "Common/Assert.h"

#include "VideoBackends/D3D/D3DBase.h" // For D3D::device
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoCommon/VR.h"          // For VR state and config
#include "VideoCommon/VideoConfig.h" // For g_ActiveConfig and StereoMode

namespace DX11
{
#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p)                                                                            \
  {                                                                                                \
    if (p)                                                                                         \
    {                                                                                              \
      (p)->Release();                                                                              \
      (p) = nullptr;                                                                               \
    }                                                                                              \
  }
#endif

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

  // Create VR eye textures if VR is active
  if (g_has_openvr && g_ActiveConfig.stereo_mode == StereoMode::OpenVR)
  {
    ASSERT_MSG(VIDEO, g_hmd_window_width > 0 && g_hmd_window_height > 0,
               "VR HMD render target size is invalid.");

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = g_hmd_window_width;  // Per-eye width from OpenVR
    tex_desc.Height = g_hmd_window_height; // Per-eye height from OpenVR
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    // Create as TYPELESS to allow for potential RTV format aliasing if DXFramebuffer::Create attempts it.
    // The main RTV will be UNORM.
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    tex_desc.SampleDesc.Count = 1;                // MSAA is handled separately if needed
    tex_desc.SampleDesc.Quality = 0;
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    tex_desc.CPUAccessFlags = 0;
    tex_desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> left_eye_d3d_tex;
    hr = D3D::device->CreateTexture2D(&tex_desc, nullptr, &left_eye_d3d_tex);
    ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create left eye VR texture: {}", DX11HRWrap(hr));
    if (FAILED(hr)) return false;
    D3DCommon::SetDebugObjectName(left_eye_d3d_tex.Get(), "Left Eye VR Texture");
    g_left_eye_texture_d3d_for_submit = left_eye_d3d_tex.Get();
    g_left_eye_texture_d3d_for_submit->AddRef(); // DXTexture::CreateAdopted will take ownership, but we need a ref for submit

    g_left_eye_dxtexture = static_cast<DXTexture*>(DXTexture::CreateAdopted(std::move(left_eye_d3d_tex)).release());
    if (!g_left_eye_dxtexture) return false;
    g_left_eye_dxframebuffer = static_cast<DXFramebuffer*>(DXFramebuffer::Create(g_left_eye_dxtexture, nullptr, {}).release());
    if (!g_left_eye_dxframebuffer) {
        delete g_left_eye_dxtexture; // This should release the adopted texture
        g_left_eye_dxtexture = nullptr;
        SAFE_RELEASE(g_left_eye_texture_d3d_for_submit);
        return false;
    }
    // The RTV is part of the DXFramebuffer.
    D3DCommon::SetDebugObjectName((IUnknown*)g_left_eye_dxframebuffer->GetRTVArray(), "Left Eye VR RTV");


    ComPtr<ID3D11Texture2D> right_eye_d3d_tex;
    hr = D3D::device->CreateTexture2D(&tex_desc, nullptr, &right_eye_d3d_tex);
    ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create right eye VR texture: {}", DX11HRWrap(hr));
    if (FAILED(hr)) {
      delete g_left_eye_dxframebuffer; g_left_eye_dxframebuffer = nullptr;
      delete g_left_eye_dxtexture; g_left_eye_dxtexture = nullptr;
      SAFE_RELEASE(g_left_eye_texture_d3d_for_submit);
      return false;
    }
    D3DCommon::SetDebugObjectName(right_eye_d3d_tex.Get(), "Right Eye VR Texture");
    g_right_eye_texture_d3d_for_submit = right_eye_d3d_tex.Get();
    g_right_eye_texture_d3d_for_submit->AddRef(); // DXTexture::CreateAdopted will take ownership

    g_right_eye_dxtexture = static_cast<DXTexture*>(DXTexture::CreateAdopted(std::move(right_eye_d3d_tex)).release());
    if (!g_right_eye_dxtexture) {
      delete g_left_eye_dxframebuffer; g_left_eye_dxframebuffer = nullptr;
      delete g_left_eye_dxtexture; g_left_eye_dxtexture = nullptr;
      SAFE_RELEASE(g_left_eye_texture_d3d_for_submit);
      SAFE_RELEASE(g_right_eye_texture_d3d_for_submit); // Release the ref we took
      return false;
    }
    g_right_eye_dxframebuffer = static_cast<DXFramebuffer*>(DXFramebuffer::Create(g_right_eye_dxtexture, nullptr, {}).release());
    if (!g_right_eye_dxframebuffer) {
      delete g_left_eye_dxframebuffer; g_left_eye_dxframebuffer = nullptr;
      delete g_left_eye_dxtexture; g_left_eye_dxtexture = nullptr;
      delete g_right_eye_dxtexture; g_right_eye_dxtexture = nullptr;
      SAFE_RELEASE(g_left_eye_texture_d3d_for_submit);
      SAFE_RELEASE(g_right_eye_texture_d3d_for_submit);
      return false;
    }
    // The RTV is part of the DXFramebuffer.
    D3DCommon::SetDebugObjectName((IUnknown*)g_right_eye_dxframebuffer->GetRTVArray(), "Right Eye VR RTV");

    INFO_LOG_FMT(VIDEO, "Successfully created VR eye DXTextures and DXFramebuffers ({}x{}).", g_hmd_window_width, g_hmd_window_height);
  }

  return true;
}

void SwapChain::DestroySwapChainBuffers()
{
  m_framebuffer.reset();
  m_texture.reset();

  // Release VR resources
  // DXFramebuffer and DXTexture are unique_ptr like, managed by delete.
  // The raw ID3D11Texture2D used for submission needs manual release.
  if (g_left_eye_dxframebuffer) { delete g_left_eye_dxframebuffer; g_left_eye_dxframebuffer = nullptr; }
  if (g_left_eye_dxtexture) { delete g_left_eye_dxtexture; g_left_eye_dxtexture = nullptr; }
  SAFE_RELEASE(g_left_eye_texture_d3d_for_submit);

  if (g_right_eye_dxframebuffer) { delete g_right_eye_dxframebuffer; g_right_eye_dxframebuffer = nullptr; }
  if (g_right_eye_dxtexture) { delete g_right_eye_dxtexture; g_right_eye_dxtexture = nullptr; }
  SAFE_RELEASE(g_right_eye_texture_d3d_for_submit);
}
}  // namespace DX11
