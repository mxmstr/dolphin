// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/VRD3D.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3D/D3DSwapChain.h" // For DXFramebuffer
#include "VideoCommon/VROpenVR.h"
#include "Common/Logging/Log.h"

VRD3D::VRD3D(VROpenVR* vr_system, ID3D11Device* d3d_device)
    : m_vr_system(vr_system),
      m_d3d_device(d3d_device),
      m_d3d_context(DX11::D3D::context.Get()), // Assuming D3D::context is available
      m_initialized(false)
{
  ASSERT(m_vr_system != nullptr);
  ASSERT(m_d3d_device != nullptr);
  ASSERT(m_d3d_context != nullptr);
}

VRD3D::~VRD3D()
{
  // Resources should be released by ComPtr and unique_ptr automatically.
  m_initialized = false;
}

bool VRD3D::Init()
{
  if (m_initialized)
    return true;

  if (!m_vr_system || !m_vr_system->IsInitialized())
  {
    ERROR_LOG_FMT(D3D, "VRD3D::Init - VROpenVR system not initialized.");
    return false;
  }

  m_vr_system->GetHMDRecommendedRenderTargetSize(&m_render_target_width, &m_render_target_height);
  if (m_render_target_width == 0 || m_render_target_height == 0)
  {
    ERROR_LOG_FMT(D3D, "VRD3D::Init - Invalid recommended render target size from OpenVR: %u x %u",
                  m_render_target_width, m_render_target_height);
    return false;
  }

  INFO_LOG_FMT(D3D, "VRD3D: Creating eye resources with size: %u x %u", m_render_target_width,
               m_render_target_height);

  // Create Color Textures for rendering
  TextureConfig color_tex_config(m_render_target_width, m_render_target_height, 1, 1,
                                 AbstractTextureFormat::RGBA8, true /* rendertarget */,
                                 false /* msaa */, 1 /* layers */);
  m_left_eye_render_texture = DX11::DXTexture::Create(color_tex_config, "VRLeftEyeRT");
  m_right_eye_render_texture = DX11::DXTexture::Create(color_tex_config, "VRRightEyeRT");

  if (!m_left_eye_render_texture || !m_right_eye_render_texture)
  {
    ERROR_LOG_FMT(D3D, "VRD3D::Init - Failed to create eye render textures.");
    return false;
  }

  // Create Depth Texture (shared for now)
  TextureConfig depth_tex_config(m_render_target_width, m_render_target_height, 1, 1,
                                 AbstractTextureFormat::D24_S8, true /* depthbuffer */,
                                 false /* msaa */, 1 /* layers */);
  m_depth_buffer_texture = DX11::DXTexture::Create(depth_tex_config, "VRDepthBuffer");
  if (!m_depth_buffer_texture)
  {
    ERROR_LOG_FMT(D3D, "VRD3D::Init - Failed to create VR depth buffer texture.");
    return false;
  }

  // Create Framebuffers
  m_left_eye_framebuffer = DX11::DXFramebuffer::Create(m_left_eye_render_texture.get(), m_depth_buffer_texture.get());
  m_right_eye_framebuffer = DX11::DXFramebuffer::Create(m_right_eye_render_texture.get(), m_depth_buffer_texture.get());

  if (!m_left_eye_framebuffer || !m_right_eye_framebuffer)
  {
    ERROR_LOG_FMT(D3D, "VRD3D::Init - Failed to create eye framebuffers.");
    return false;
  }

  // Get the underlying D3D textures for submission to OpenVR.
  // These must be the same textures used by the framebuffers.
  ID3D11Resource* left_res = m_left_eye_render_texture->GetD3DTexture();
  ID3D11Resource* right_res = m_right_eye_render_texture->GetD3DTexture();

  if (!left_res || !right_res)
  {
    ERROR_LOG_FMT(D3D, "VRD3D::Init - Failed to get D3DResource from eye render textures.");
    return false;
  }

  HRESULT hr_left = left_res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)m_left_eye_d3d_texture_for_submit.GetAddressOf());
  HRESULT hr_right = right_res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)m_right_eye_d3d_texture_for_submit.GetAddressOf());

  if (FAILED(hr_left) || FAILED(hr_right))
  {
    ERROR_LOG_FMT(D3D, "VRD3D::Init - Failed to QueryInterface ID3D11Texture2D from eye render textures.");
    m_left_eye_d3d_texture_for_submit.Reset();
    m_right_eye_d3d_texture_for_submit.Reset();
    return false;
  }

  m_initialized = true;
  INFO_LOG_FMT(D3D, "VRD3D initialized successfully with eye framebuffers and textures.");
  return true;
}

bool VRD3D::BeginFrame()
{
  if (!m_initialized || !m_vr_system)
    return false;

  // vr::VRCompositor()->WaitGetPoses can be called here if needed for synchronization.
  // For now, relying on VROpenVR::GetHMDPose called by Gfx.
  return true;
}

bool VRD3D::SubmitFrames()
{
  if (!m_initialized || !m_vr_system || !m_vr_system->GetCompositor())
  {
    ERROR_LOG_FMT(D3D, "VRD3D::SubmitFrames - Not initialized or VR compositor not available.");
    return false;
  }

  if (!m_left_eye_d3d_texture_for_submit || !m_right_eye_d3d_texture_for_submit)
  {
    ERROR_LOG_FMT(D3D, "VRD3D::SubmitFrames - D3D Eye textures for submission are not valid.");
    return false;
  }

  vr::Texture_t left_eye_texture = {m_left_eye_d3d_texture_for_submit.Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};
  vr::Texture_t right_eye_texture = {m_right_eye_d3d_texture_for_submit.Get(), vr::TextureType_DirectX, vr::ColorSpace_Auto};

  // Default bounds: Whole texture.
  vr::VRTextureBounds_t texture_bounds;
  texture_bounds.uMin = 0.0f;
  texture_bounds.vMin = 0.0f;
  texture_bounds.uMax = 1.0f;
  texture_bounds.vMax = 1.0f;

  vr::EVRCompositorError error_left = m_vr_system->GetCompositor()->Submit(vr::Eye_Left, &left_eye_texture, &texture_bounds);
  if (error_left != vr::VRCompositorError_None)
  {
    ERROR_LOG_FMT(D3D, "VRD3D::SubmitFrames - Failed to submit left eye: %s", vr::VRCompositor()->GetCompositorErrorNameFromEnum(error_left));
    // Continue to attempt right eye submission.
  }

  vr::EVRCompositorError error_right = m_vr_system->GetCompositor()->Submit(vr::Eye_Right, &right_eye_texture, &texture_bounds);
  if (error_right != vr::VRCompositorError_None)
  {
    ERROR_LOG_FMT(D3D, "VRD3D::SubmitFrames - Failed to submit right eye: %s", vr::VRCompositor()->GetCompositorErrorNameFromEnum(error_right));
    return false; // If right eye fails, consider the whole submission a failure for now.
  }

  // vr::VRCompositor()->PostPresentHandoff(); // Deprecated in newer OpenVR SDKs.
  // If using an older SDK, this might be needed. For current OpenVR, Submit takes care of it.

  // D3D::context->Flush(); // Recommended by OpenVR docs after submit if not using explicit timing.

  return error_left == vr::VRCompositorError_None && error_right == vr::VRCompositorError_None;
}

DX11::DXTexture* VRD3D::GetLeftEyeTexture()
{
  // This texture is the one used for rendering, which is then submitted.
  return m_initialized ? m_left_eye_render_texture.get() : nullptr;
}

DX11::DXTexture* VRD3D::GetRightEyeTexture()
{
  // This texture is the one used for rendering, which is then submitted.
  return m_initialized ? m_right_eye_render_texture.get() : nullptr;
}

DX11::DXFramebuffer* VRD3D::GetLeftEyeFramebuffer()
{
  return m_initialized ? m_left_eye_framebuffer.get() : nullptr;
}

DX11::DXFramebuffer* VRD3D::GetRightEyeFramebuffer()
{
  return m_initialized ? m_right_eye_framebuffer.get() : nullptr;
}


void VRD3D::GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height)
{
  if (m_initialized)
  {
    *width = m_render_target_width;
    *height = m_render_target_height;
  }
  else if (m_vr_system && m_vr_system->IsInitialized())
  {
     m_vr_system->GetHMDRecommendedRenderTargetSize(width, height);
  }
  else
  {
    *width = 0;
    *height = 0;
  }
}
