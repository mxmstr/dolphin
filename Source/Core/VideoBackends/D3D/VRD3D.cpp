// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoBackends/D3D/VRD3D.h"

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DGfx.h"
#include "VideoBackends/D3D/D3DSwapChain.h"
#include "VideoBackends/D3D/DXTexture.h"

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR.h"
#include <openvr.h>
// VROculus.h and OculusSystemLibraryHeader.h includes are already removed from VR.h
// VROpenVR.h is included via VR.h if HAVE_OPENVR is defined

// Forward declaration for g_renderer (already present)
namespace DX11
{
class Gfx;
}
extern std::unique_ptr<AbstractGfx> g_gfx;

namespace DX11
{

#ifdef HAVE_OPENVR
// Globals for OpenVR eye textures and their render target views
ComPtr<ID3D11Texture2D> m_left_texture = nullptr;
ComPtr<ID3D11Texture2D> m_right_texture = nullptr;
ComPtr<ID3D11RenderTargetView> m_left_texture_rtv = nullptr;
ComPtr<ID3D11RenderTargetView> m_right_texture_rtv = nullptr;
#endif

void GetEyeTextureDimensions(int eye, UINT* width, UINT* height)
{
    if (!width || !height)
    {
        ERROR_LOG_FMT(VR, "GetEyeTextureDimensions: width or height pointer is null");
        return;
    }
    *width = 0; *height = 0;

#ifdef HAVE_OPENVR
    if (g_has_openvr && vr::VRSystem()) // Check if VRSystem is initialized
    {
        vr::VRSystem()->GetRecommendedRenderTargetSize(width, height);
        return;
    }
#endif
    // Fallback if no VR system provided dimensions or OpenVR init failed
    if (*width == 0 || *height == 0) {
        WARN_LOG_FMT(VR, "GetEyeTextureDimensions: Could not determine VR dimensions. Defaulting to EFB size.");
        *width = g_framebuffer_manager->GetEFBWidth();
        *height = g_framebuffer_manager->GetEFBHeight();
    }
}

void VR_ConfigureHMD()
{
  // This function was primarily for Oculus SDK specific configurations.
  // OpenVR configuration is typically handled during InitOpenVR and by the runtime itself.
#ifdef HAVE_OPENVR
  if (g_has_openvr && vr::VRCompositor())
  {
    // Any OpenVR D3D specific configurations that need to happen after device creation
    // but before rendering starts would go here.
    // For example, some applications might set explicit timing modes:
    // vr::VRCompositor()->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
    // However, this is often not necessary for basic integration.
  }
#endif
}

void VR_StartFramebuffer()
{
#ifdef HAVE_OPENVR
  if (g_has_openvr && D3D::device)
  {
    UINT eye_width, eye_height;
    GetEyeTextureDimensions(0, &eye_width, &eye_height); // Get recommended size for one eye

    if (eye_width == 0 || eye_height == 0)
    {
        ERROR_LOG_FMT(VR, "VR_StartFramebuffer: Invalid eye texture dimensions ({}x{})", eye_width, eye_height);
        return;
    }

    TextureConfig tex_config(
        eye_width, eye_height, 1, 1,
        g_ActiveConfig.iMultisamples, AbstractTextureFormat::RGBA8, // Assuming RGBA8, format might need to be more flexible
        AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2D);

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = tex_config.width;
    tex_desc.Height = tex_config.height;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    // OpenVR expects textures in sRGB format for gamma correction unless specified otherwise.
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    tex_desc.SampleDesc.Count = tex_config.samples; // Use MSAA samples from config
    tex_desc.SampleDesc.Quality = 0; // Default quality
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    tex_desc.CPUAccessFlags = 0;
    tex_desc.MiscFlags = 0;

    HRESULT hr_left = D3D::device->CreateTexture2D(&tex_desc, nullptr, &m_left_texture);
    HRESULT hr_right = D3D::device->CreateTexture2D(&tex_desc, nullptr, &m_right_texture);

    ASSERT_MSG(VR, SUCCEEDED(hr_left) && SUCCEEDED(hr_right), "Failed to create OpenVR eye textures");

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
    // RTV format MUST match the texture format if the texture is not typeless.
    // For SRGB textures, the RTV must also be SRGB.
    rtv_desc.Format = tex_desc.Format; // Use the texture's format (DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    rtv_desc.ViewDimension = (tex_config.samples > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;

    HRESULT hr_rtv_left = E_FAIL, hr_rtv_right = E_FAIL;

    if (SUCCEEDED(hr_left) && m_left_texture) // Ensure texture was created
    {
      hr_rtv_left = D3D::device->CreateRenderTargetView(m_left_texture.Get(), &rtv_desc, &m_left_texture_rtv);
      if (FAILED(hr_rtv_left))
      {
        ERROR_LOG_FMT(VR, "Failed to create RTV for left eye texture. HRESULT: {:#08x}", static_cast<unsigned int>(hr_rtv_left));
        m_left_texture.Reset(); // Release texture if RTV creation failed
      }
    }
    if (SUCCEEDED(hr_right) && m_right_texture) // Ensure texture was created
    {
      hr_rtv_right = D3D::device->CreateRenderTargetView(m_right_texture.Get(), &rtv_desc, &m_right_texture_rtv);
      if (FAILED(hr_rtv_right))
      {
        ERROR_LOG_FMT(VR, "Failed to create RTV for right eye texture. HRESULT: {:#08x}", static_cast<unsigned int>(hr_rtv_right));
        m_right_texture.Reset(); // Release texture if RTV creation failed
      }
    }

    // Optional: Add an assert or further error handling if RTVs are still null
    ASSERT_MSG(VR, m_left_texture_rtv && m_right_texture_rtv, "Failed to create OpenVR eye RTVs");
  }
#endif
}

void VR_StopFramebuffer()
{
#if defined(HAVE_OPENVR)
  if (g_has_openvr)
  {
    m_left_texture.Reset();
    m_right_texture.Reset();
    m_left_texture_rtv.Reset();
    m_right_texture_rtv.Reset();
  }
#endif
}

void VR_BeginFrame()
{
  // OpenVR's frame lifecycle is typically WaitGetPoses -> Submit.
  // No specific "BeginFrame" call is usually needed for OpenVR itself here.
  // Any per-frame setup for OpenVR (like getting predicted poses) happens in VR_UpdateHeadTrackingIfNeeded.
}

void VR_RenderToEyebuffer(int eye, int hmd_number /*= 0 This param is no longer used */)
{
#if defined(HAVE_OPENVR)
  if (g_has_openvr && D3D::context)
  {
    ID3D11RenderTargetView* rtv = (eye == 0) ? m_left_texture_rtv.Get() : m_right_texture_rtv.Get();
    if (rtv)
    {
      // Assuming a global or EFB depth buffer is used.
      // If each eye texture needs its own depth buffer, it should be created in VR_StartFramebuffer
      // and bound here.
      /*auto* dx_tex = static_cast<DXTexture*>(g_framebuffer_manager->GetEFBDepthTexture());
      ID3D11DepthStencilView* dsv = dx_tex ?
                                    dx_tex->GetDSV() : nullptr;
      D3D::context->OMSetRenderTargets(1, &rtv, dsv);*/
      if (g_framebuffer_manager->GetEFBFramebuffer())
       {
         auto* dx_fb = static_cast<DXFramebuffer*>(g_framebuffer_manager->GetEFBFramebuffer());
         D3D::context->OMSetRenderTargets(1, &rtv, dx_fb->GetDSV());
       }
    }
    else
    {
       WARN_LOG_FMT(VR, "VR_RenderToEyebuffer: RTV for eye {} is null.", eye);
       // Fallback: render to EFB? This might not be correct for stereo.
       // This indicates an issue with VR_StartFramebuffer or resource management.
       if (g_framebuffer_manager->GetEFBFramebuffer())
       {
         auto* dx_fb = static_cast<DXFramebuffer*>(g_framebuffer_manager->GetEFBFramebuffer());
         D3D::context->OMSetRenderTargets(dx_fb->GetNumRTVs(), dx_fb->GetRTVArray(), dx_fb->GetDSV());
       }
    }
  }
#endif
}

void VR_PresentHMDFrame()
{
  DX11::Gfx* g_gfx_dx11 = static_cast<DX11::Gfx*>(g_gfx.get());

#ifdef HAVE_OPENVR
  if (g_has_openvr && vr::VRCompositor() && m_left_texture && m_right_texture)
  {
    // Submit textures to OpenVR compositor
    vr::Texture_t leftEyeTexture = {m_left_texture.Get(), vr::TextureType_DirectX, vr::ColorSpace_Gamma};
    vr::EVRCompositorError eErrorLeft = vr::VRCompositor()->Submit(vr::Eye_Left, &leftEyeTexture);
    if (eErrorLeft != vr::VRCompositorError_None)
        WARN_LOG_FMT(VR, "Failed to submit left eye to OpenVR: {}", static_cast<int>(eErrorLeft));

    vr::Texture_t rightEyeTexture = {m_right_texture.Get(), vr::TextureType_DirectX, vr::ColorSpace_Gamma};
    vr::EVRCompositorError eErrorRight = vr::VRCompositor()->Submit(vr::Eye_Right, &rightEyeTexture);
     if (eErrorRight != vr::VRCompositorError_None)
        WARN_LOG_FMT(VR, "Failed to submit right eye to OpenVR: {}", static_cast<int>(eErrorRight));

    // Mirroring logic
    if (g_ActiveConfig.iMirrorStyle != VR_MIRROR_DISABLED &&
        g_ActiveConfig.iMirrorPlayer != VR_PLAYER_NONE && g_gfx_dx11 && g_gfx_dx11->GetSwapChain())
    {
      DX11::SwapChain* swap_chain = static_cast<DX11::SwapChain*>(g_gfx_dx11->GetSwapChain());

      if (swap_chain->GetFramebuffer())
      {
          ID3D11Texture2D* src_tex_for_mirror = nullptr;
          // Determine which eye texture to use for mirroring
          if (g_ActiveConfig.iMirrorStyle == VR_MIRROR_RIGHT && m_right_texture)
            src_tex_for_mirror = m_right_texture.Get();
          else if (m_left_texture) // Default to left eye for VR_MIRROR_LEFT, VR_MIRROR_BOTH, VR_MIRROR_WARPED (simplified)
            src_tex_for_mirror = m_left_texture.Get();

          DXFramebuffer* backbuffer_fb = swap_chain->GetFramebuffer();
          ID3D11Texture2D* dst_tex_for_mirror = nullptr;
          if (backbuffer_fb && backbuffer_fb->GetColorAttachment())
             dst_tex_for_mirror = static_cast<DXTexture*>(backbuffer_fb->GetColorAttachment())->GetD3DTexture();

          if (src_tex_for_mirror && dst_tex_for_mirror)
          {
              // Simple copy for now. A proper mirror might involve scaling or drawing a quad.
              // If VR_MIRROR_BOTH is selected, this would ideally show both eyes side-by-side.
              // That requires more complex rendering than a simple CopyResource.
              D3D::context->CopyResource(dst_tex_for_mirror, src_tex_for_mirror);
          }
          swap_chain->Present();
      }
    }
  }
#endif
}

void VR_DrawTimewarpFrame()
{
  // OpenVR handles timewarp (asynchronous reprojection) primarily through its compositor.
  // A manual "DrawTimewarpFrame" is typically not implemented by applications using OpenVR's compositor.
  // If the old "OpcodeReplay" feature (which this function was tied to) is still desired,
  // it would need significant rework to integrate with OpenVR's frame submission and timing.
  // For now, this function is empty as standard OpenVR behavior is assumed.
}

} // namespace DX11
