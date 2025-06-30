// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/VRD3D.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3D/D3DSwapChain.h" // For DXFramebuffer
#include "VideoCommon/VROpenVR.h"
#include "Common/Logging/Log.h"
#include <Common/Assert.h>

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
    ERROR_LOG_FMT(VR, "VRD3D::Init - VROpenVR system not initialized.");
    return false;
  }

  m_vr_system->GetHMDRecommendedRenderTargetSize(&m_render_target_width, &m_render_target_height);
  if (m_render_target_width == 0 || m_render_target_height == 0)
  {
    ERROR_LOG_FMT(VR, "VRD3D::Init - Invalid recommended render target size from OpenVR: {} x {}",
                  m_render_target_width, m_render_target_height);
    return false;
  }

  INFO_LOG_FMT(VR, "VRD3D: Creating eye resources with size: {} x {}", m_render_target_width,
               m_render_target_height);

  // Create D3D11 Textures for OpenVR Submission (these are the final ones OpenVR sees)
  D3D11_TEXTURE2D_DESC submit_tex_desc = {};
  submit_tex_desc.Width = m_render_target_width;
  submit_tex_desc.Height = m_render_target_height;
  submit_tex_desc.MipLevels = 1;
  submit_tex_desc.ArraySize = 1;
  submit_tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // Common format
  submit_tex_desc.SampleDesc.Count = 1;
  submit_tex_desc.Usage = D3D11_USAGE_DEFAULT;
  // BindFlags must include D3D11_BIND_SHADER_RESOURCE for OpenVR,
  // and D3D11_BIND_RENDER_TARGET if we were to render directly (but we won't for these).
  // It also needs to be a copy destination.
  submit_tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; // OpenVR needs to read from it.
                                                          // D3D11_BIND_RENDER_TARGET is not strictly needed if only copying.
  submit_tex_desc.CPUAccessFlags = 0;
  // MiscFlags: D3D11_RESOURCE_MISC_SHARED for sharing with OpenVR compositor.
  // This is crucial. OpenVR docs mention this.
  submit_tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;


  HRESULT hr = m_d3d_device->CreateTexture2D(&submit_tex_desc, nullptr, m_left_eye_d3d_texture_for_submit.GetAddressOf());
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VR, "VRD3D::Init - Failed to create left eye D3D texture for submission. HRESULT: {:#x}", hr);
    return false;
  }
  hr = m_d3d_device->CreateTexture2D(&submit_tex_desc, nullptr, m_right_eye_d3d_texture_for_submit.GetAddressOf());
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VR, "VRD3D::Init - Failed to create right eye D3D texture for submission. HRESULT: {:#x}", hr);
    return false;
  }
  INFO_LOG_FMT(VR, "VRD3D::Init - Created D3D textures for OpenVR submission.");

  // Create Intermediate D3D11 Textures for Dolphin Rendering (these are rendered to by Dolphin)
  D3D11_TEXTURE2D_DESC intermediate_tex_desc = {};
  intermediate_tex_desc.Width = m_render_target_width;
  intermediate_tex_desc.Height = m_render_target_height;
  intermediate_tex_desc.MipLevels = 1;
  intermediate_tex_desc.ArraySize = 1;
  // Use TYPELESS format to allow for aliasing with integer RTVs for EFB logic ops.
  intermediate_tex_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  intermediate_tex_desc.SampleDesc.Count = 1;
  intermediate_tex_desc.Usage = D3D11_USAGE_DEFAULT;
  // These need to be render targets. They also need to be copy sources.
  intermediate_tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; // Shader resource if Dolphin needs to read from it for effects, etc.
  intermediate_tex_desc.CPUAccessFlags = 0;
  intermediate_tex_desc.MiscFlags = 0; // No D3D11_RESOURCE_MISC_SHARED needed unless also directly submitted

  hr = m_d3d_device->CreateTexture2D(&intermediate_tex_desc, nullptr, m_intermediate_left_eye_d3d_texture.GetAddressOf());
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VR, "VRD3D::Init - Failed to create intermediate left eye D3D texture. HRESULT: {:#x}", hr);
    return false;
  }
  hr = m_d3d_device->CreateTexture2D(&intermediate_tex_desc, nullptr, m_intermediate_right_eye_d3d_texture.GetAddressOf());
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VR, "VRD3D::Init - Failed to create intermediate right eye D3D texture. HRESULT: {:#x}", hr);
    return false;
  }
  INFO_LOG_FMT(VR, "VRD3D::Init - Created intermediate D3D textures for Dolphin rendering.");

  // Define the TextureConfig for the intermediate wrappers.
  // This should match how the intermediate_tex_desc was set up for the D3D textures.
  TextureConfig intermediate_wrapper_config(
      m_render_target_width, m_render_target_height, 1, /* levels */
      1,                                               /* layers */
      1,                                               /* samples */
      AbstractTextureFormat::RGBA8,                    // Matches DXGI_FORMAT_R8G8B8A8_UNORM
      AbstractTextureFlag_RenderTarget,                // Only RenderTarget flag is needed here from TextureConfig's perspective for this use case.
                                                       // The D3D texture itself has D3D11_BIND_SHADER_RESOURCE.
      AbstractTextureType::Texture_2D);

  // Wrap the intermediate D3D textures with Dolphin's DXTexture wrappers using the new CreateAdopted overload
  m_left_eye_render_texture_intermediate_wrapper = DX11::DXTexture::CreateAdopted(
      m_intermediate_left_eye_d3d_texture, intermediate_wrapper_config, "VRLeftEyeIntermediateWrapped");
  m_right_eye_render_texture_intermediate_wrapper = DX11::DXTexture::CreateAdopted(
      m_intermediate_right_eye_d3d_texture, intermediate_wrapper_config, "VRRightEyeIntermediateWrapped");

  if (!m_left_eye_render_texture_intermediate_wrapper || !m_right_eye_render_texture_intermediate_wrapper)
  {
    ERROR_LOG_FMT(VR, "VRD3D::Init - Failed to create DXTexture wrappers for intermediate textures via new CreateAdopted.");
    return false;
  }
  INFO_LOG_FMT(VR, "VRD3D::Init - Created DXTexture wrappers for intermediate textures using new CreateAdopted.");

  // Create Depth Texture (shared for now, and used with intermediate framebuffers)
  TextureConfig depth_tex_config(m_render_target_width, m_render_target_height, 1 /*levels*/, 1 /*layers*/,
                                 1 /*samples*/, AbstractTextureFormat::D24_S8,
                                 AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2D);
  m_depth_buffer_texture = DX11::DXTexture::Create(depth_tex_config, "VRDepthBuffer");
  if (!m_depth_buffer_texture)
  {
    ERROR_LOG_FMT(VR, "VRD3D::Init - Failed to create VR depth buffer texture.");
    return false;
  }
  INFO_LOG_FMT(VR, "VRD3D::Init - Created shared depth buffer.");

  // Create Framebuffers for rendering to intermediate textures
  m_left_eye_framebuffer_intermediate = DX11::DXFramebuffer::Create(m_left_eye_render_texture_intermediate_wrapper.get(), m_depth_buffer_texture.get(), {});
  m_right_eye_framebuffer_intermediate = DX11::DXFramebuffer::Create(m_right_eye_render_texture_intermediate_wrapper.get(), m_depth_buffer_texture.get(), {});

  if (!m_left_eye_framebuffer_intermediate || !m_right_eye_framebuffer_intermediate)
  {
    ERROR_LOG_FMT(VR, "VRD3D::Init - Failed to create intermediate eye framebuffers.");
    return false;
  }
  INFO_LOG_FMT(VR, "VRD3D::Init - Created intermediate framebuffers.");


  // Get the underlying D3D textures for submission to OpenVR.
  // These are now m_left_eye_d3d_texture_for_submit and m_right_eye_d3d_texture_for_submit
  // which are already ComPtr<ID3D11Texture2D>. No QueryInterface needed if created correctly.
  // ID3D11Resource* left_res = m_left_eye_render_texture->GetD3DTexture(); // Old way
  // ID3D11Resource* right_res = m_right_eye_render_texture->GetD3DTexture(); // Old way

  // Validate that the submission textures were created.
  if (!m_left_eye_d3d_texture_for_submit || !m_right_eye_d3d_texture_for_submit)
  {
    ERROR_LOG_FMT(VR, "VRD3D::Init - D3D textures for submission are null after creation attempt.");
    return false;
  }

  // The ComPtrs m_left_eye_d3d_texture_for_submit and m_right_eye_d3d_texture_for_submit
  // are already the ID3D11Texture2D types we need.

  m_initialized = true;
  INFO_LOG_FMT(VR, "VRD3D initialized successfully with intermediate and submission textures/framebuffers.");
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
  INFO_LOG_FMT(VR, "VRD3D::SubmitFrames called.");
  if (!m_initialized || !m_vr_system || !m_vr_system->GetCompositor())
  {
    ERROR_LOG_FMT(VR, "VRD3D::SubmitFrames - Not initialized or VR compositor not available.");
    return false;
  }

  // WaitGetPoses is NO LONGER CALLED HERE. It's called by the dedicated VR presentation thread.
  // This function now assumes that the m_left_eye_d3d_texture_for_submit and
  // m_right_eye_d3d_texture_for_submit have already been populated (e.g., by CopyResource)
  // by the presentation thread.

  ID3D11Texture2D* left_submit_tex_ptr = m_left_eye_d3d_texture_for_submit.Get();
  ID3D11Texture2D* right_submit_tex_ptr = m_right_eye_d3d_texture_for_submit.Get();

  if (!left_submit_tex_ptr || !right_submit_tex_ptr)
  {
    ERROR_LOG_FMT(VR, "VRD3D::SubmitFrames - D3D Eye textures for submission are not valid. Left: {}, Right: {}",
                  (void*)left_submit_tex_ptr, (void*)right_submit_tex_ptr);
    return false;
  }
  
  INFO_LOG_FMT(VR, "VRD3D::SubmitFrames - Submitting textures. Left: {}, Right: {}",
               (void*)left_submit_tex_ptr, (void*)right_submit_tex_ptr);

  vr::Texture_t left_eye_texture = {left_submit_tex_ptr, vr::TextureType_DirectX, vr::ColorSpace_Auto};
  vr::Texture_t right_eye_texture = {right_submit_tex_ptr, vr::TextureType_DirectX, vr::ColorSpace_Auto};

  // Default bounds: Whole texture.
  vr::VRTextureBounds_t texture_bounds;
  texture_bounds.uMin = 0.0f;
  texture_bounds.vMin = 0.0f;
  texture_bounds.uMax = 1.0f;
  texture_bounds.vMax = 1.0f;

  vr::EVRCompositorError error_left = m_vr_system->GetCompositor()->Submit(vr::Eye_Left, &left_eye_texture, &texture_bounds);
  if (error_left != vr::VRCompositorError_None)
  {
    // It would be ideal to have a helper in VROpenVR to get error string from m_vr_system->GetEnglishStringForErrorCode(error) or similar
    ERROR_LOG_FMT(VR, "VRD3D::SubmitFrames - Failed to submit left eye: {}", static_cast<int>(error_left));
    // Continue to attempt right eye submission.
  }

  vr::EVRCompositorError error_right = m_vr_system->GetCompositor()->Submit(vr::Eye_Right, &right_eye_texture, &texture_bounds);
  if (error_right != vr::VRCompositorError_None)
  {
    ERROR_LOG_FMT(VR, "VRD3D::SubmitFrames - Failed to submit right eye: {}", static_cast<int>(error_right));
    return false; // If right eye fails, consider the whole submission a failure for now.
  }
  
  if (error_left == vr::VRCompositorError_None && error_right == vr::VRCompositorError_None)
  {
    INFO_LOG_FMT(VR, "VRD3D::SubmitFrames - Both eyes submitted successfully to OpenVR compositor.");
  }

  // vr::VRCompositor()->PostPresentHandoff(); // Deprecated in newer OpenVR SDKs.
  // If using an older SDK, this might be needed. For current OpenVR, Submit takes care of it.

  // D3D::context->Flush(); // Recommended by OpenVR docs after submit if not using explicit timing.

  return error_left == vr::VRCompositorError_None && error_right == vr::VRCompositorError_None;
}

DX11::DXTexture* VRD3D::GetLeftEyeTexture()
{
  // This texture is the one used for rendering, which is then submitted.
  // This should now return the WRAPPER for the INTERMEDIATE texture.
  return m_initialized ? m_left_eye_render_texture_intermediate_wrapper.get() : nullptr;
}

DX11::DXTexture* VRD3D::GetRightEyeTexture()
{
  // This texture is the one used for rendering, which is then submitted.
  // This should now return the WRAPPER for the INTERMEDIATE texture.
  return m_initialized ? m_right_eye_render_texture_intermediate_wrapper.get() : nullptr;
}

DX11::DXFramebuffer* VRD3D::GetLeftEyeFramebuffer()
{
  // This should now return the INTERMEDIATE framebuffer.
  return m_initialized ? m_left_eye_framebuffer_intermediate.get() : nullptr;
}

DX11::DXFramebuffer* VRD3D::GetRightEyeFramebuffer()
{
  // This should now return the INTERMEDIATE framebuffer.
  return m_initialized ? m_right_eye_framebuffer_intermediate.get() : nullptr;
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
