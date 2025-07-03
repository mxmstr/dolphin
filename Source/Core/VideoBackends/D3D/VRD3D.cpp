// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/VRD3D.h"

#include "Common/Logging/Log.h"
#include "Common/Timer.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D/DXTexture.h"
#include "VideoBackends/D3D/DXShader.h" // For pixel shaders if needed for blitting/mirroring
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/RenderState.h" // For SamplerState

// Assuming OpenVR is the primary target. Oculus SDK specifics might be conditional.
#ifdef HAVE_OPENVR_SDK
// openvr.h is included from VRD3D.h
#endif

namespace DX11
{

// These would be file-static if not accessed from elsewhere, or part of a VR context struct
#ifdef HAVE_OPENVR_SDK
static vr::Texture_t openvr_eye_textures[2] = {};
#endif

// --- Actual VRD3D implementation ---

void VR_D3D_ConfigureHMD()
{
#ifdef HAVE_OPENVR_SDK
  if (g_has_openvr && VR_GetHMD()) // VR_GetHMD() would be from VideoCommon/VR.h
  {
    // OpenVR doesn't have a D3D-specific configure call like older Oculus SDKs.
    // Most configuration is done during VR_Init (in VideoCommon/VR.cpp)
    // and by setting up the submission textures correctly.
    // Ensure necessary OpenVR subsystems are initialized if D3D specific setup is ever needed.
    INFO_LOG(VR_D3D, "OpenVR HMD configured for D3D.");
  }
#endif
  // Add Oculus specific configuration if dual SDK support is intended and HAVE_OCULUS_SDK is defined
}

void VR_D3D_StartFramebuffer(DX11::Gfx* gfx_context)
{
  if (!gfx_context || !g_has_hmd)
    return;

  // Based on Hydra's FramebufferManager constructor and VR_StartFramebuffer
  gfx_context->m_stereo3d = true;
  gfx_context->m_eye_count = 2; // Default for stereo VR

  // Create eye textures that will be submitted to the HMD
  // These are distinct from the EFB.
  TextureConfig tex_config = {};
  tex_config.width = g_renderer->GetTargetWidth();    // Target width for each eye
  tex_config.height = g_renderer->GetTargetHeight();   // Target height for each eye
  tex_config.layers = 1;
  tex_config.levels = 1;
  tex_config.format = AbstractTextureFormat::R8G8B8A8_UNORM; // Typical format for HMD submission
  tex_config.bind_flags = AbstractTextureFlag::RenderTarget | AbstractTextureFlag::ShaderResource;
  tex_config.usage = AbstractTextureUsage::Default;
  tex_config.msaa_samples = 1; // MSAA is usually resolved before submission to HMD

  for (int i = 0; i < 2; ++i)
  {
    if (gfx_context->m_frontBuffer[i])
    {
      delete gfx_context->m_frontBuffer[i];
      gfx_context->m_frontBuffer[i] = nullptr;
    }

    std::string name = StringFromFormat("VREyeTexture%d", i);
    gfx_context->m_frontBuffer[i] = static_cast<DXTexture*>(DXTexture::Create(tex_config, name).release());

    if (!gfx_context->m_frontBuffer[i] || !gfx_context->m_frontBuffer[i]->GetRawTexView())
    {
      PanicAlert("Failed to create D3D eye texture %d for VR.", i);
      // Consider more robust error handling, like disabling VR.
      return;
    }

#ifdef HAVE_OPENVR_SDK
    if (g_has_openvr)
    {
      openvr_eye_textures[i].handle = gfx_context->m_frontBuffer[i]->GetRawTexView(); // ID3D11Texture2D*
      openvr_eye_textures[i].eType = vr::TextureType_DirectX;
      openvr_eye_textures[i].eColorSpace = vr::ColorSpace_Gamma; // Or Auto / Linear depending on workflow
    }
#endif
  }
  INFO_LOG(VR_D3D, "VR Framebuffers (eye textures) started.");
}

void VR_D3D_StopFramebuffer(DX11::Gfx* gfx_context)
{
  if (!gfx_context)
    return;

  for (int i = 0; i < 2; ++i)
  {
    delete gfx_context->m_frontBuffer[i];
    gfx_context->m_frontBuffer[i] = nullptr;
#ifdef HAVE_OPENVR_SDK
    openvr_eye_textures[i].handle = nullptr;
#endif
  }
  gfx_context->m_stereo3d = false;
  gfx_context->m_eye_count = 1;
  INFO_LOG(VR_D3D, "VR Framebuffers (eye textures) stopped.");
}

void VR_D3D_RenderToEyeBuffer(DX11::Gfx* gfx_context, int eye)
{
  if (!gfx_context || !gfx_context->m_stereo3d || eye < 0 || eye >= gfx_context->m_eye_count || !gfx_context->m_frontBuffer[eye])
    return;

  // Set the render target to the specified eye's texture
  // The DXFramebuffer would be implicitly created/managed if we use AbstractGfx::SetFramebuffer.
  // For direct control, similar to Hydra:
  ID3D11RenderTargetView* rtv = gfx_context->m_frontBuffer[eye]->GetRTV();
  // Depth buffer would typically be shared or also per-eye if needed, managed by FramebufferManager or Gfx.
  // For simplicity, assuming EFB's depth for now, or null if eye textures don't need their own depth.
  // This needs to match how the scene is intended to be rendered for VR.
  // If eye textures are just for final blit, they might not need a depth buffer here.
  // If scene is rendered directly to eye textures, they need a depth buffer.

  // For now, let's assume we are rendering the EFB content to the eye buffers, so no depth needed for eye buffer itself.
  // If rendering scene directly, a depth buffer associated with m_frontBuffer[eye] would be used.
  D3D::stateman->SetRenderTargets(1, &rtv, nullptr);
}

void VR_D3D_SubmitFrameToHMD()
{
#ifdef HAVE_OPENVR_SDK
  if (g_has_openvr && VR_GetHMD() && vr::VRCompositor())
  {
    vr::EVRCompositorError err;
    err = vr::VRCompositor()->Submit(vr::Eye_Left, &openvr_eye_textures[0]);
    if (err != vr::VRCompositorError_None)
      WARN_LOG(VR_D3D, "Failed to submit left eye to OpenVR compositor: %d", err);

    err = vr::VRCompositor()->Submit(vr::Eye_Right, &openvr_eye_textures[1]);
    if (err != vr::VRCompositorError_None)
      WARN_LOG(VR_D3D, "Failed to submit right eye to OpenVR compositor: %d", err);

    // WaitGetPoses is often called after submit to sync for the next frame.
    // This might be handled in VideoCommon/VR.cpp or D3DGfx::PresentBackbuffer
    // vr::VRCompositor()->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
  }
#endif
  // Add Oculus submission logic if HAVE_OCULUS_SDK defined and g_has_oculus
}

void VR_D3D_DrawTimewarpFrame(DX11::Gfx* gfx_context)
{
  if (!gfx_context || !g_ActiveConfig.bEnableVR || !g_has_hmd)
    return;

  // This function in Hydra's D3D Render.cpp calls VR_DrawTimewarpFrame (from VRCommon/VR.cpp),
  // which then calls ovrHmd_EndFrame with existing eye textures and new poses for Oculus SDK 0.5.
  // For OpenVR or newer Oculus SDKs, the approach is different.
  // OpenVR: Re-submit the same textures with new poses (if supported directly, usually not).
  //         More commonly, you'd re-render the scene or use SDK's async reprojection.
  // Oculus SDK 0.6+: Similar to OpenVR, rely on async timewarp or re-render.
  // The Hydra "Synchronous Timewarp" was a custom solution.

  // If we are to replicate Hydra's synchronous timewarp (which re-issues draw calls),
  // that logic would be in VideoCommon/Fifo.cpp or similar, not D3D-specific.
  // If it's just submitting the already rendered eye textures with new pose data:

#ifdef HAVE_OPENVR_SDK
  if (g_has_openvr && VR_GetHMD() && vr::VRCompositor())
  {
    // OpenVR's basic Submit doesn't inherently support re-submitting with new poses for timewarp
    // in the same way older Oculus SDKs did with ovrHmd_EndFrame.
    // True timewarp is handled by the compositor. This function might be a no-op
    // or would need to trigger a re-render if that's the desired "synchronous timewarp" behavior.
    // For now, let's assume it's for a scenario where the SDK supports this kind of re-projection.
    // This is effectively another VSync-timed submission.

    // VR_GetEyePoses(); // Get latest poses, should be done in VR.cpp before calling this
    // VR_D3D_SubmitFrameToHMD();
    // The above would just re-submit the same frame.
    // Hydra's VR_DrawTimewarpFrame in VRD3D.cpp essentially called VR_PresentHMDFrame again.
    // which for oculus 0.5 was ovrHmd_EndFrame(hmd, g_eye_poses, &g_eye_texture[0].Texture);
    // This implies the SDK handled timewarp internally with new poses.

    // If the goal is to simulate Hydra's behavior:
    // 1. Get new eye poses (done in VR.cpp or Gfx::SwapImpl)
    // 2. Submit the *existing* eye textures again.
    VR_D3D_SubmitFrameToHMD(); // This would re-submit the textures that were last rendered.
    // INFO_LOG(VR_D3D, "Timewarp frame submitted (D3D).");

  }
#endif
  // Add Oculus specific timewarp submission if different and supported.
}

} // namespace DX11
