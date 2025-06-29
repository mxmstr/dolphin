// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <openvr.h> // For vr::VRCompositorError, vr::Texture_t, vr::VRTextureBounds_t
#include <d3d11.h>    // For D3D11 types
#include <wrl/client.h> // For ComPtr

#include "Common/Matrix.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoCommon/VROpenVR.h"

class VRD3D
{
public:
  VRD3D(VROpenVR* vr_system, ID3D11Device* d3d_device);
  ~VRD3D();

  bool Init(); // Initialize eye textures based on HMD recommended size

  // Called at the beginning of a VR frame
  bool BeginFrame();

  // Submits the rendered eye textures to the HMD
  bool SubmitFrames();

  // Getters for eye textures (which are D3D render targets)
  // Getters for eye textures (which are D3D render targets)
  // These DXTextures are primarily for OpenVR submission. Framebuffers below are for rendering.
  DX11::DXTexture* GetLeftEyeTexture();
  DX11::DXTexture* GetRightEyeTexture();

  // Getters for Framebuffers for rendering
  DX11::DXFramebuffer* GetLeftEyeFramebuffer();
  DX11::DXFramebuffer* GetRightEyeFramebuffer();

  // Get recommended render target size
  void GetRecommendedRenderTargetSize(uint32_t* width, uint32_t* height);


private:
  VROpenVR* m_vr_system; // Raw pointer, lifecycle managed externally
  ID3D11Device* m_d3d_device; // Raw pointer to the D3D device
  ID3D11DeviceContext* m_d3d_context; // Raw pointer to the D3D device context

  // Eye textures for OpenVR
  Microsoft::WRL::ComPtr<ID3D11Texture2D> m_left_eye_d3d_texture_for_submit;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> m_right_eye_d3d_texture_for_submit;

  // Dolphin's abstract texture and framebuffer wrappers for rendering
  std::unique_ptr<DX11::DXTexture> m_left_eye_render_texture;
  std::unique_ptr<DX11::DXTexture> m_right_eye_render_texture;
  std::unique_ptr<DX11::DXTexture> m_depth_buffer_texture; // Optional: could be shared or per-eye

  std::unique_ptr<DX11::DXFramebuffer> m_left_eye_framebuffer;
  std::unique_ptr<DX11::DXFramebuffer> m_right_eye_framebuffer;

  uint32_t m_render_target_width = 0;
  uint32_t m_render_target_height = 0;

  bool m_initialized = false;
};
