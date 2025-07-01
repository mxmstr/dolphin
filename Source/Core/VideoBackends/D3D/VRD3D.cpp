// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoBackends/D3D/VRD3D.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DState.h" // For D3D::SetPointCopySampler, D3D::drawShadedTexQuad
#include "VideoBackends/D3D/D3DTexture.h"
#include "VideoBackends/D3D/D3DUtil.h"
#include "VideoCommon/FramebufferManager.h" // Needs to be included before D3D/FramebufferManager.h
#include "VideoBackends/D3D/FramebufferManager.h" // For D3D-specific FramebufferManager access if any (DEPRECATED)
#include "VideoCommon/ShaderCache.h"         // For g_shader_cache
#include "VideoCommon/AbstractGfx.h"       // For g_gfx
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VertexShaderManager.h" // For VertexShaderCache - though likely through g_shader_cache
#include "VideoCommon/PixelShaderManager.h"  // For PixelShaderCache - though likely through g_shader_cache
#include "VideoCommon/GeometryShaderManager.h" // For GeometryShaderCache - though likely through g_shader_cache
#include "VideoCommon/OnScreenDisplay.h"   // For OSD
#include "VideoCommon/Renderer.h"          // For g_renderer
#include "VideoCommon/VR.h"
#include "VideoCommon/VROculus.h"
#include "VideoCommon/VROpenVR.h"
#include "VideoCommon/VideoConfig.h"

// TODO: These includes are from Hydra's Render.cpp, might not all be needed here
// or might need to be accessed via g_renderer or g_shader_cache
// #include "VideoBackends/D3D/PixelShaderCache.h"
// #include "VideoBackends/D3D/VertexShaderCache.h"
// #include "VideoBackends/D3D/GeometryShaderCache.h"


namespace DX11
{
// Oculus Rift
#ifdef OVR_MAJOR_VERSION

#if OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5
ovrD3D11Texture g_eye_texture[2]; // This is for very old Oculus SDKs
#else
//------------------------------------------------------------
// ovrSwapTextureSet wrapper class that also maintains the render target views
// needed for D3D11 rendering.
struct OculusTexture
{
#if OVR_PRODUCT_VERSION >= 1
  ovrTextureSwapChain TextureChain;
  std::vector<ID3D11RenderTargetView*> TexRtv;
  // clean up member COM pointers
  template <typename T>
  void Release(T*& obj)
  {
    if (!obj)
      return;
    obj->Release();
    obj = nullptr;
  }
#else
  ovrSwapTextureSet* TextureSet;
  ID3D11RenderTargetView* TexRtv[3];
#endif

  OculusTexture(ovrHmd hmd0, ovrSizei size)
  {
    D3D11_TEXTURE2D_DESC dsDesc;
    dsDesc.Width = size.w;
    dsDesc.Height = size.h;
    dsDesc.MipLevels = 1;
    dsDesc.ArraySize = 1;
    dsDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    dsDesc.SampleDesc.Count = 1;  // No multi-sampling allowed
    dsDesc.SampleDesc.Quality = 0;
    dsDesc.Usage = D3D11_USAGE_DEFAULT;
    dsDesc.CPUAccessFlags = 0;
    dsDesc.MiscFlags = 0;
    dsDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    int length = 0;
    ovrResult res;
#if OVR_PRODUCT_VERSION >= 1
    TextureChain = nullptr;
    ovrTextureSwapChainDesc desc = {};
    desc.Type = ovrTexture_2D;
    desc.ArraySize = 1;
    desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.Width = size.w;
    desc.Height = size.h;
    desc.MipLevels = 1;
    desc.SampleCount = 1;
    desc.MiscFlags = ovrTextureMisc_DX_Typeless;
    desc.BindFlags = ovrTextureBind_DX_RenderTarget;
    desc.StaticImage = ovrFalse;

    res = ovr_CreateTextureSwapChainDX(hmd0, DX11::D3D::device, &desc, &TextureChain);
    ovr_GetTextureSwapChainLength(hmd0, TextureChain, &length);
    if (!OVR_SUCCESS(res))
    {
      ovrErrorInfo e;
      ovr_GetLastErrorInfo(&e);
      PanicAlert(
          "ovr_CreateTextureSwapChainDX(hmd, OVR_FORMAT_R8G8B8A8_UNORM_SRGB, %d, %d)=%d failed\n%s",
          size.w, size.h, res, e.ErrorString);
      return;
    }
#elif OVR_MAJOR_VERSION >= 7
    unsigned int miscFlags = ovrSwapTextureSetD3D11_Typeless;  // ovrSwapTextureSetD3D11_Typeless
                                                               // just causes a black screen on both
                                                               // the mirror and the HMD
    res = ovr_CreateSwapTextureSetD3D11(hmd0, DX11::D3D::device, &dsDesc, miscFlags, &TextureSet);
    length = TextureSet->TextureCount;
#else
    res = ovrHmd_CreateSwapTextureSetD3D11(hmd0, DX11::D3D::device, &dsDesc, &TextureSet);
    length = TextureSet->TextureCount;
#endif
    for (int i = 0; i < length; ++i)
    {
#if OVR_PRODUCT_VERSION >= 1
      ID3D11Texture2D* tex = nullptr;
      ovr_GetTextureSwapChainBufferDX(hmd0, TextureChain, i, IID_PPV_ARGS(&tex));
      D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
      rtvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      ID3D11RenderTargetView* rtv;
      DX11::D3D::device->CreateRenderTargetView(tex, &rtvd, &rtv);
      TexRtv.push_back(rtv);
      tex->Release();
#else
      ovrD3D11Texture* tex = (ovrD3D11Texture*)&TextureSet->Textures[i];
#if OVR_MAJOR_VERSION >= 7
      D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
      rtvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      DX11::D3D::device->CreateRenderTargetView(tex->D3D11.pTexture, &rtvd, &TexRtv[i]);
#else
      DX11::D3D::device->CreateRenderTargetView(tex->D3D11.pTexture, nullptr, &TexRtv[i]);
#endif
#endif
    }
  }

#if OVR_PRODUCT_VERSION >= 1
  ID3D11RenderTargetView* GetRTV()
  {
    int index = 0;
    ovr_GetTextureSwapChainCurrentIndex(hmd, TextureChain, &index);
    return TexRtv[index];
  }
#endif

  // Commit changes
  void Commit()
  {
#if OVR_PRODUCT_VERSION >= 1
    ovr_CommitTextureSwapChain(hmd, TextureChain);
#endif
  }

  void AdvanceToNextTexture()
  {
#if OVR_PRODUCT_VERSION == 0
    TextureSet->CurrentIndex = (TextureSet->CurrentIndex + 1) % TextureSet->TextureCount;
#endif
  }

  void Release(ovrHmd hmd0)
  {
#if OVR_PRODUCT_VERSION >= 1
    for (int i = 0; i < (int)TexRtv.size(); ++i)
    {
      Release(TexRtv[i]);
    }
    if (TextureChain)
    {
      ovr_DestroyTextureSwapChain(hmd0, TextureChain);
    }
#else
    ovrHmd_DestroySwapTextureSet(hmd0, TextureSet);
#endif
  }
};

OculusTexture* pEyeRenderTexture[2];
ovrRecti eyeRenderViewport[2];
#if OVR_PRODUCT_VERSION >= 1
ovrMirrorTexture mirrorTexture = nullptr;
#else
ovrTexture* mirrorTexture = nullptr;
#endif
int mirror_width = 0, mirror_height = 0;
D3D11_TEXTURE2D_DESC texdesc = {};
#endif

#endif

#ifdef HAVE_OPENVR
ID3D11Texture2D* m_left_texture = nullptr;
ID3D11Texture2D* m_right_texture = nullptr;
#endif

void VR_ConfigureHMD()
{
#ifdef HAVE_OPENVR
  if (g_has_openvr && m_pCompositor)
  {
    // m_pCompositor->SetGraphicsDevice(vr::Compositor_DeviceType_DirectX, nullptr);
  }
#endif
#ifdef OVR_MAJOR_VERSION
  if (g_has_rift)
  {
#if OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5
    ovrD3D11Config cfg;
    cfg.D3D11.Header.API = ovrRenderAPI_D3D11;
#ifdef OCULUSSDK044ORABOVE
    cfg.D3D11.Header.BackBufferSize.w = hmdDesc.Resolution.w;
    cfg.D3D11.Header.BackBufferSize.h = hmdDesc.Resolution.h;
#else
    cfg.D3D11.Header.RTSize.w = hmdDesc.Resolution.w;
    cfg.D3D11.Header.RTSize.h = hmdDesc.Resolution.h;
#endif
    cfg.D3D11.Header.Multisample = 0;
    cfg.D3D11.pDevice = D3D::device;
    cfg.D3D11.pDeviceContext = D3D::context;
    cfg.D3D11.pSwapChain = D3D::swapchain;
    cfg.D3D11.pBackBufferRT = D3D::GetBackBuffer()->GetRTV();
    if (g_is_direct_mode)  // If Rift is in Direct Mode
    {
      // To do: This is a bit of a hack, but I haven't found any problems with this.
      // If we don't want to do this, large changes will be needed to init sequence.
      D3D::UnloadDXGI();  // Unload CreateDXGIFactory() before ovrHmd_AttachToWindow, or else direct
                          // mode won't work.
      ovrHmd_AttachToWindow(hmd, D3D::hWnd, nullptr, nullptr);  // Attach to Direct Mode.
      D3D::LoadDXGI();
    }
    int caps = 0;
#if OVR_MAJOR_VERSION <= 4
    if (g_Config.bChromatic)
      caps |= ovrDistortionCap_Chromatic;
#endif
    if (g_Config.bTimewarp)
      caps |= ovrDistortionCap_TimeWarp;
    if (g_Config.bVignette)
      caps |= ovrDistortionCap_Vignette;
    if (g_Config.bNoRestore)
      caps |= ovrDistortionCap_NoRestore;
    if (g_Config.bFlipVertical)
      caps |= ovrDistortionCap_FlipInput;
    if (g_Config.bSRGB)
      caps |= ovrDistortionCap_SRGB;
    if (g_Config.bOverdrive)
      caps |= ovrDistortionCap_Overdrive;
    if (g_Config.bHqDistortion)
      caps |= ovrDistortionCap_HqDistortion;
    ovrHmd_ConfigureRendering(hmd, &cfg.Config, caps, g_eye_fov, g_eye_render_desc);
#if OVR_MAJOR_VERSION <= 4
    ovrhmd_EnableHSWDisplaySDKRender(hmd, false);  // Disable Health and Safety Warning.
#endif
#else
    for (int i = 0; i < ovrEye_Count; ++i)
      g_eye_render_desc[i] = ovrHmd_GetRenderDesc(hmd, (ovrEyeType)i, g_eye_fov[i]);
#endif
  }
#endif
}

#if defined(OVR_MAJOR_VERSION) && (OVR_PRODUCT_VERSION >= 1 || OVR_MAJOR_VERSION >= 6)
void RecreateMirrorTextureIfNeeded()
{
  int w = 128;
  int h = 128;
  if (g_renderer)
  {
    w = g_renderer->GetBackbufferWidth();
    h = g_renderer->GetBackbufferHeight();
  }
  bool bNoMirrorToWindow = g_ActiveConfig.iMirrorPlayer == VR_PLAYER_NONE ||
                           g_ActiveConfig.iMirrorStyle == VR_MIRROR_DISABLED;
  if (w != mirror_width || h != mirror_height || ((mirrorTexture == nullptr) != bNoMirrorToWindow))
  {
    if (mirrorTexture)
    {
      ovrHmd_DestroyMirrorTexture(hmd, mirrorTexture);
      mirrorTexture = nullptr;
    }
    if (!bNoMirrorToWindow)
    {
#if OVR_PRODUCT_VERSION >= 1
      // Create a mirror, to see Rift output on a monitor
      mirrorTexture = nullptr;
      ovrMirrorTextureDesc desc = {};
      // desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB;
      desc.Format = OVR_FORMAT_R8G8B8A8_UNORM;
      desc.Width = w;
      desc.Height = h;
      mirror_width = w;
      mirror_height = h;
      ovrResult result = ovr_CreateMirrorTextureDX(hmd, D3D::device, &desc, &mirrorTexture);
#else
      // Create a mirror to see on the monitor.
      texdesc.ArraySize = 1;
      texdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
      texdesc.Width = w;
      texdesc.Height = h;
      texdesc.Usage = D3D11_USAGE_DEFAULT;
      texdesc.SampleDesc.Count = 1;
      texdesc.MipLevels = 1;
      mirror_width = texdesc.Width;
      mirror_height = texdesc.Height;
      mirrorTexture = nullptr;
#if OVR_MAJOR_VERSION >= 7
      unsigned int miscFlags =
          ovrSwapTextureSetD3D11_Typeless;  // could also be ovrSwapTextureSetD3D11_Typeless
      ovrResult result =
          ovr_CreateMirrorTextureD3D11(hmd, D3D::device, &texdesc, miscFlags, &mirrorTexture);
#else
      ovrResult result =
          ovrHmd_CreateMirrorTextureD3D11(hmd, D3D::device, &texdesc, &mirrorTexture);
#endif
#endif
      if (!OVR_SUCCESS(result))
      {
        ERROR_LOG(VR, "Failed to create D3D mirror texture. Error: %d", result);
        mirrorTexture = nullptr;
      }
    }
  }
}
#endif

void VR_StartFramebuffer()
{
#if defined(OVR_MAJOR_VERSION) && (OVR_PRODUCT_VERSION >= 1 || OVR_MAJOR_VERSION >= 6)
  if (g_has_rift)
  {
    // On Oculus SDK 0.6.0 and above, get Oculus to create our textures for us. And remember the
    // viewport.
    for (int eye = 0; eye < 2; eye++)
    {
      ovrSizei target_size;
      target_size.w = FramebufferManager::m_target_width;
      target_size.h = FramebufferManager::m_target_height;
      pEyeRenderTexture[eye] = new OculusTexture(hmd, target_size);
      eyeRenderViewport[eye].Pos.x = 0;
      eyeRenderViewport[eye].Pos.y = 0;
      eyeRenderViewport[eye].Size = target_size;
    }
    RecreateMirrorTextureIfNeeded();
  }
#endif
  if (g_has_vr920)
  {
#ifdef _WIN32
    VR920_StartStereo3D();
#endif
  }
#if (defined(OVR_MAJOR_VERSION) && OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5) ||          \
    defined(HAVE_OPENVR)
  // This section is for very old Oculus SDKs (0.5 and below) and OpenVR from Hydra
  // We will adapt it for current OpenVR using the new FramebufferManager members.
  if (g_has_openvr && g_ActiveConfig.stereo_mode != StereoMode::OculusVR && g_framebuffer_manager && g_gfx)
  {
    // Create eye textures for OpenVR and store them in FramebufferManager
    VideoCommon::TextureConfig vr_eye_texture_config = VideoCommon::TextureConfig(
        g_framebuffer_manager->GetEFBWidth(), g_framebuffer_manager->GetEFBHeight(), 1, 1, // Each eye texture is a single layer/slice
        g_ActiveConfig.iMultisamples, VideoCommon::FramebufferManager::GetEFBColorFormat(),
        VideoCommon::AbstractTextureFlag_RenderTarget | VideoCommon::AbstractTextureFlag_ShaderResource,
        VideoCommon::AbstractTextureType::Texture_2D);

    // Casting g_framebuffer_manager to its concrete type or adding a method
    // to set these textures would be cleaner. For now, direct assignment if compatible.
    // This assumes m_vr_eye_textures in FramebufferManager is accessible and is of type std::unique_ptr<AbstractTexture>
    // This was already prepared in FramebufferManager.cpp, but we ensure creation here.
    // The actual texture pointers are now stored in g_framebuffer_manager->m_vr_eye_textures
    // So, the old FramebufferManager::m_efb.m_frontBuffer is no longer used for this.

    // The actual creation was moved to FramebufferManager::CreateEFBFramebuffer,
    // here we just assign to m_left_texture/m_right_texture for OpenVR's Submit path.
    // This part of Hydra's VR_StartFramebuffer was creating D3DTexture2D and assigning to
    // FramebufferManager::m_efb.m_frontBuffer.
    // The new FramebufferManager creates AbstractTextures in its m_vr_eye_textures.
    // We need to get these and assign to m_left_texture/m_right_texture.

    // Ensure eye textures are created in FramebufferManager
    // This is a bit redundant if CreateEFBFramebuffer already does it, but good for explicit control.
    // g_framebuffer_manager->CreateVREyeTextures(); // Assuming such a method exists or is part of RecreateEFBFramebuffer

    // For OpenVR, m_left_texture and m_right_texture are used by VR_PresentHMDFrame
    // These should point to the D3D resources held by g_framebuffer_manager->m_vr_eye_textures
    VideoCommon::AbstractTexture* left_abs_tex = g_framebuffer_manager->GetVREyeTexture(0);
    VideoCommon::AbstractTexture* right_abs_tex = g_framebuffer_manager->GetVREyeTexture(1);

    if (left_abs_tex && right_abs_tex)
    {
        // We need the underlying ID3D11Texture2D. DXTexture should provide a way to get it.
        // This assumes GetVREyeTexture returns a pointer that can be dynamic_cast to DXTexture.
        DXTexture* dx_left_tex = dynamic_cast<DXTexture*>(left_abs_tex);
        DXTexture* dx_right_tex = dynamic_cast<DXTexture*>(right_abs_tex);

        if (dx_left_tex && dx_right_tex)
        {
            m_left_texture = dx_left_tex->GetD3DTexture(); // Assuming DXTexture has GetD3DTexture()
            m_right_texture = dx_right_tex->GetD3DTexture();
            // AddRef if GetD3DTexture doesn't, though ComPtr in DXTexture should handle lifetime.
            // If GetD3DTexture returns a raw pointer from a ComPtr, AddRef might be needed if m_left_texture is also a raw pointer.
            // However, m_left_texture is ID3D11Texture2D*, so direct assignment is fine if lifetimes are managed.
            // For safety, if these are just raw pointers tracking resources owned by FramebufferManager,
            // no AddRef/Release cycle is needed here, but they become dangling if FramebufferManager releases them.
            // Given m_left_texture and m_right_texture are global in this file, this is risky.
            // It's better if VR_PresentHMDFrame directly gets them from g_framebuffer_manager.
            // For now, let's keep them as they are in Hydra for minimal changes to VR_PresentHMDFrame's OpenVR path.
            // We'll assume g_framebuffer_manager outlives their use in a frame.
        }
        else
        {
            PanicAlert("Failed to cast VR eye textures to DXTexture for OpenVR.");
        }
    }
    else
    {
        PanicAlert("VR eye textures not created in FramebufferManager for OpenVR.");
    }
  }
#endif
}

void VR_StopFramebuffer()
{
#if defined(OVR_MAJOR_VERSION) && (OVR_PRODUCT_VERSION >= 1 || OVR_MAJOR_VERSION >= 6)
  if (mirrorTexture)
  {
    ovrHmd_DestroyMirrorTexture(hmd, mirrorTexture);
    mirrorTexture = nullptr;
  }
  // On Oculus SDK 0.6.0 and above, we need to destroy the eye textures Oculus created for us.
  for (int eye = 0; eye < 2; eye++)
  {
    if (pEyeRenderTexture[eye])
    {
      pEyeRenderTexture[eye]->Release(hmd);
      delete pEyeRenderTexture[eye];
      pEyeRenderTexture[eye] = nullptr;
    }
  }
#endif
// Old Oculus SDK and OpenVR Hydra textures were released here.
// New m_vr_eye_textures in FramebufferManager are handled by its destructor / DestroyEFBFramebuffer.
// The global m_left_texture/m_right_texture for OpenVR do not need explicit release here
// if they are just pointers to textures managed by FramebufferManager.
#if defined(HAVE_OPENVR)
  if (g_has_openvr)
  {
    // These were pointers to textures owned by FramebufferManager::m_efb.m_frontBuffer in Hydra
    // Now they will point to textures owned by g_framebuffer_manager->m_vr_eye_textures
    // No SAFE_RELEASE needed here if they are just non-owning pointers.
    m_left_texture = nullptr;
    m_right_texture = nullptr;
  }
#endif
}

void VR_BeginFrame()
{
// At the start of a frame, we get the frame timing and begin the frame.
#ifdef OVR_MAJOR_VERSION
  if (g_has_rift)
  {
#if OVR_PRODUCT_VERSION >= 1 || OVR_MAJOR_VERSION >= 6
    RecreateMirrorTextureIfNeeded();
    ++g_ovr_frameindex;
// On Oculus SDK 0.6.0 and above, we get the frame timing manually, then swap each eye texture
#if OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 7
    g_rift_frame_timing = ovrHmd_GetFrameTiming(hmd, 0);
#endif
    for (int eye = 0; eye < 2; eye++)
    {
      // Increment to use next texture, just before writing
      pEyeRenderTexture[eye]->AdvanceToNextTexture();
    }
#else
    ovrHmd_DismissHSWDisplay(hmd);
    g_rift_frame_timing = ovrHmd_BeginFrame(hmd, ++g_ovr_frameindex);
#endif
  }
#endif
}

void VR_RenderToEyebuffer(int eye, int hmd_number)
{
#ifdef OVR_MAJOR_VERSION
  if (g_has_rift && (hmd_number == 1 || !g_has_openvr))
  {
#if OVR_PRODUCT_VERSION >= 1
    ID3D11RenderTargetView* rtv = pEyeRenderTexture[eye]->GetRTV();
    D3D::context->OMSetRenderTargets(1, &rtv, nullptr);
    rtv = nullptr;
#elif OVR_MAJOR_VERSION >= 6
    D3D::context->OMSetRenderTargets(
        1, &pEyeRenderTexture[eye]->TexRtv[pEyeRenderTexture[eye]->TextureSet->CurrentIndex],
        nullptr);
#else
    D3D::context->OMSetRenderTargets(1, &FramebufferManager::m_efb.m_frontBuffer[eye]->GetRTV(),
                                     nullptr);
#endif
  }
#endif
#if defined(HAVE_OPENVR)
  if (g_has_openvr && (hmd_number == 0 || !g_has_rift)) // If OpenVR is primary or Rift is not present
  {
    if (g_framebuffer_manager)
    {
      VideoCommon::AbstractTexture* abstract_eye_texture = g_framebuffer_manager->GetVREyeTexture(eye);
      if (abstract_eye_texture)
      {
        DXTexture* dx_eye_texture = static_cast<DXTexture*>(abstract_eye_texture);
        ID3D11RenderTargetView* rtv = dx_eye_texture->GetD3DRenderTargetView();
        // The depth buffer of the main EFB is typically used/bound by the Gfx context.
        // For VR eye rendering, we only set the color target. Depth testing should use the EFB depth.
        // If separate depth per eye is needed, FramebufferManager would need to manage that too.
        ID3D11DepthStencilView* dsv = nullptr;
        if (g_framebuffer_manager->GetEFBDepthTexture())
        {
            dsv = static_cast<DXTexture*>(g_framebuffer_manager->GetEFBDepthTexture())->GetD3DDepthStencilView();
        }
        D3D::context->OMSetRenderTargets(1, &rtv, dsv);
      }
      else
      {
        ERROR_LOG(VR, "OpenVR: Eye texture %d is null in FramebufferManager.", eye);
      }
    }
    else
    {
      ERROR_LOG(VR, "OpenVR: g_framebuffer_manager is null in VR_RenderToEyebuffer.");
    }
  }
#endif
}

void VR_PresentHMDFrame()
{
#ifdef HAVE_OPENVR
  if (m_pCompositor && g_framebuffer_manager)
  {
    VideoCommon::AbstractTexture* abs_left_eye_tex = g_framebuffer_manager->GetVREyeTexture(0);
    VideoCommon::AbstractTexture* abs_right_eye_tex = g_framebuffer_manager->GetVREyeTexture(1);

    if (abs_left_eye_tex && abs_right_eye_tex)
    {
      ID3D11Texture2D* d3d_left_tex = static_cast<DXTexture*>(abs_left_eye_tex)->GetD3DTexture();
      ID3D11Texture2D* d3d_right_tex = static_cast<DXTexture*>(abs_right_eye_tex)->GetD3DTexture();

      if (d3d_left_tex && d3d_right_tex)
      {
        vr::Texture_t leftEyeTexture = {d3d_left_tex, vr::TextureType_DirectX, vr::ColorSpace_Gamma};
        vr::VRCompositor()->Submit(vr::Eye_Left, &leftEyeTexture);
        vr::Texture_t rightEyeTexture = {d3d_right_tex, vr::TextureType_DirectX, vr::ColorSpace_Gamma};
        vr::VRCompositor()->Submit(vr::Eye_Right, &rightEyeTexture);
      }
      else
      {
        ERROR_LOG(VR, "OpenVR: Failed to get D3D textures for submission.");
      }
    }
    else
    {
      ERROR_LOG(VR, "OpenVR: Eye textures are null in FramebufferManager for submission.");
    }

    m_pCompositor->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    g_older_tracking_time = g_old_tracking_time;
    g_old_tracking_time = g_last_tracking_time;
    g_last_tracking_time = Common::Timer::GetTimeMs() / 1000.0;
    if (g_ActiveConfig.iMirrorStyle != VR_MIRROR_DISABLED &&
        g_ActiveConfig.iMirrorPlayer != VR_PLAYER_NONE)
    {
      TargetRectangle sourceRc;
      sourceRc.left = 0;
      sourceRc.top = 0;
      sourceRc.right = g_renderer->GetTargetWidth();
      sourceRc.bottom = g_renderer->GetTargetHeight();

      D3D::context->OMSetRenderTargets(1, &D3D::GetBackBuffer()->GetRTV(), nullptr);
      D3D11_VIEWPORT vp =
          CD3D11_VIEWPORT((float)0, (float)0, (float)g_renderer->GetBackbufferWidth(),
                          (float)g_renderer->GetBackbufferHeight());
      // warped or both eyes
      int eye = 0;
      if (g_ActiveConfig.iMirrorStyle >= VR_MIRROR_WARPED)
        vp.Width *= 0.5f;
      else
        eye = g_ActiveConfig.iMirrorStyle - VR_MIRROR_LEFT;

      D3D::context->RSSetViewports(1, &vp);
      VideoCommon::AbstractTexture* mirror_source_tex_abs = g_framebuffer_manager->GetVREyeTexture(eye);
      if (mirror_source_tex_abs)
      {
        DXTexture* mirror_source_dx_tex = static_cast<DXTexture*>(mirror_source_tex_abs);
        D3D::drawShadedTexQuad(mirror_source_dx_tex->GetD3DShaderResourceView(),
                               sourceRc.AsRECT(), sourceRc.GetWidth(), sourceRc.GetHeight(),
                               g_shader_cache->GetTextureCopyPixelShader(), // Uses ShaderCache
                               g_shader_cache->GetScreenQuadVertexShader(), // Uses ShaderCache
                               nullptr, // InputLayout might be part of pipeline state now
                               nullptr); // Geometry Shader
      }


      if (g_ActiveConfig.iMirrorStyle >= VR_MIRROR_WARPED)
      {
        vp.TopLeftX += vp.Width;
        D3D::context->RSSetViewports(1, &vp);
        VideoCommon::AbstractTexture* mirror_source_tex_abs_right = g_framebuffer_manager->GetVREyeTexture(1);
        if (mirror_source_tex_abs_right)
        {
            DXTexture* mirror_source_dx_tex_right = static_cast<DXTexture*>(mirror_source_tex_abs_right);
            D3D::drawShadedTexQuad(mirror_source_dx_tex_right->GetD3DShaderResourceView(),
                                   sourceRc.AsRECT(), sourceRc.GetWidth(), sourceRc.GetHeight(),
                                   g_shader_cache->GetTextureCopyPixelShader(),
                                   g_shader_cache->GetScreenQuadVertexShader(),
                                   nullptr, nullptr);
        }
      }

      // D3D::context->CopyResource(D3D::GetBackBuffer()->GetTex(), tex->D3D11.pTexture);
      D3D::swapchain->Present(0, 0);
    }
  }
#endif
#ifdef OVR_MAJOR_VERSION
  if (g_has_rift)
  {
#if OVR_PRODUCT_VERSION >= 1
    pEyeRenderTexture[0]->Commit();
    pEyeRenderTexture[1]->Commit();
#endif
    // ovrHmd_EndEyeRender(hmd, ovrEye_Left, g_left_eye_pose,
    // &FramebufferManager::m_eye_texture[ovrEye_Left].Texture);
    // ovrHmd_EndEyeRender(hmd, ovrEye_Right, g_right_eye_pose,
    // &FramebufferManager::m_eye_texture[ovrEye_Right].Texture);

    // Change to compatible D3D Blend State:
    // Some games (e.g. Paper Mario) do not use a Blend State that is compatible
    // with the Oculus Rift's SDK.  They set RenderTargetWriteMask to 0,
    // which masks out the call's Pixel Shader stage.  This also seems inefficient
    // from a rendering point of view.  Could this be an area Dolphin could be optimized?
    // To Do: Only use this when needed?  Is this slow?
    ID3D11BlendState* g_pOculusRiftBlendState = NULL;

    D3D11_BLEND_DESC oculusBlendDesc;
    ZeroMemory(&oculusBlendDesc, sizeof(D3D11_BLEND_DESC));
    oculusBlendDesc.AlphaToCoverageEnable = FALSE;
    oculusBlendDesc.IndependentBlendEnable = FALSE;
    oculusBlendDesc.RenderTarget[0].BlendEnable = FALSE;
    oculusBlendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr = D3D::device->CreateBlendState(&oculusBlendDesc, &g_pOculusRiftBlendState);
    if (FAILED(hr))
      PanicAlert("Failed to create blend state at %s %d\n", __FILE__, __LINE__);
    D3D::SetDebugObjectName((ID3D11DeviceChild*)g_pOculusRiftBlendState,
                            "blend state used to make sure rift draw call works");

    D3D::context->OMSetBlendState(g_pOculusRiftBlendState, NULL, 0xFFFFFFFF);

#if OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5
    // Let OVR do distortion rendering, Present and flush/sync.
    ovrHmd_EndFrame(hmd, g_eye_poses, &g_eye_texture[0].Texture);
#else
    ovrLayerEyeFov ld;
    ld.Header.Type = ovrLayerType_EyeFov;
    ld.Header.Flags = (g_ActiveConfig.bFlipVertical ? ovrLayerFlag_TextureOriginAtBottomLeft : 0) |
                      (g_ActiveConfig.bHqDistortion ? ovrLayerFlag_HighQuality : 0);
    for (int eye = 0; eye < 2; eye++)
    {
#if OVR_PRODUCT_VERSION >= 1
      ld.ColorTexture[eye] = pEyeRenderTexture[eye]->TextureChain;
#else
      ld.ColorTexture[eye] = pEyeRenderTexture[eye]->TextureSet;
#endif
      ld.Viewport[eye] = eyeRenderViewport[eye];
      ld.Fov[eye] = g_eye_fov[eye];
      ld.RenderPose[eye] = g_eye_poses[eye];
    }
    ovrLayerHeader* layers = &ld.Header;
    // ovrResult result =
    ovrHmd_SubmitFrame(hmd, 0, nullptr, &layers, 1);

    // Render mirror
    if (mirrorTexture && g_ActiveConfig.iMirrorPlayer != VR_PLAYER_NONE &&
        g_ActiveConfig.iMirrorStyle != VR_MIRROR_DISABLED)
    {
      if (g_ActiveConfig.iMirrorStyle == VR_MIRROR_WARPED)
      {
#if OVR_PRODUCT_VERSION >= 1
        ID3D11Texture2D* tex = nullptr;
        ovr_GetMirrorTextureBufferDX(hmd, mirrorTexture, IID_PPV_ARGS(&tex));
        D3D::context->CopyResource(D3D::GetBackBuffer()->GetTex(), tex);
        tex->Release();
#else
        ovrD3D11Texture* tex = (ovrD3D11Texture*)mirrorTexture;
        TargetRectangle sourceRc;
        sourceRc.left = 0;
        sourceRc.top = 0;
        sourceRc.right = mirror_width;
        sourceRc.bottom = mirror_height;

        D3D::context->OMSetRenderTargets(1, &D3D::GetBackBuffer()->GetRTV(), nullptr);
        D3D11_VIEWPORT vp =
            CD3D11_VIEWPORT((float)0, (float)0, (float)mirror_width, (float)mirror_height);
        D3D::context->RSSetViewports(1, &vp);
        D3D::drawShadedTexQuad(tex->D3D11.pSRView, sourceRc.AsRECT(), mirror_width, mirror_height,
                               PixelShaderCache::GetColorCopyProgram(false),
                               VertexShaderCache::GetSimpleVertexShader(),
                               VertexShaderCache::GetSimpleInputLayout(), nullptr);
#endif
      }
      else
      {
        int w = g_renderer->GetTargetWidth();
        int h = g_renderer->GetTargetHeight();
        int bbw = g_renderer->GetBackbufferWidth();
        int bbh = g_renderer->GetBackbufferHeight();
        // warped or both eyes
        int eye = 0;
        if (g_ActiveConfig.iMirrorStyle >= VR_MIRROR_WARPED)
          bbw /= 2;
        else
          eye = g_ActiveConfig.iMirrorStyle - VR_MIRROR_LEFT;

        TargetRectangle sourceRc;
        sourceRc.left = 0;
        sourceRc.top = 0;
        sourceRc.right = w;
        sourceRc.bottom = h;

        D3DTexture2D* tex = FramebufferManager::GetResolvedEFBColorTexture();

        D3D::context->OMSetRenderTargets(1, &D3D::GetBackBuffer()->GetRTV(), nullptr);
        D3D11_VIEWPORT vp = CD3D11_VIEWPORT((float)0, (float)0, (float)bbw, (float)bbh);
        D3D::context->RSSetViewports(1, &vp);
        D3D::drawShadedTexQuad(tex->GetSRV(), sourceRc.AsRECT(), w, h,
                               PixelShaderCache::GetColorCopyProgram(false),
                               VertexShaderCache::GetSimpleVertexShader(),
                               VertexShaderCache::GetSimpleInputLayout(), nullptr, 1.0f, eye);

        if (g_ActiveConfig.iMirrorStyle >= VR_MIRROR_WARPED)
        {
          vp.TopLeftX += vp.Width;
          D3D::context->RSSetViewports(1, &vp);
          D3D::drawShadedTexQuad(tex->GetSRV(), sourceRc.AsRECT(), w, h,
                                 PixelShaderCache::GetColorCopyProgram(false),
                                 VertexShaderCache::GetSimpleVertexShader(),
                                 VertexShaderCache::GetSimpleInputLayout(), nullptr, 1.0f, 1);
        }
      }
      // D3D::context->CopyResource(D3D::GetBackBuffer()->GetTex(), tex->D3D11.pTexture);
      D3D::swapchain->Present(0, 0);
    }
#endif
  }
#endif
}

void VR_DrawTimewarpFrame()
{
#ifdef OVR_MAJOR_VERSION
  if (g_has_rift)
  {
#if OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5
    ovrFrameTiming frameTime;
    frameTime = ovrHmd_BeginFrame(hmd, ++g_ovr_frameindex);
    // const ovrTexture* new_eye_texture = new
    // ovrTexture(FramebufferManager::m_eye_texture[0].Texture);
    // ovrD3D11Texture new_eye_texture;
    // memcpy((void*)&new_eye_texture, &FramebufferManager::m_eye_texture[0],
    // sizeof(ovrD3D11Texture));

    // ovrPosef new_eye_poses[2];
    // memcpy((void*)&new_eye_poses, g_eye_poses, sizeof(ovrPosef)*2);

    ovr_WaitTillTime(frameTime.NextFrameSeconds - g_ActiveConfig.fTimeWarpTweak);

    ovrHmd_EndFrame(hmd, g_eye_poses, &g_eye_texture[0].Texture);
#else
    ++g_ovr_frameindex;
#if OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 7
    // On Oculus SDK 0.6.0 and above, we get the frame timing manually, then swap each eye texture
    ovrFrameTiming frameTime;
    frameTime = ovrHmd_GetFrameTiming(hmd, 0);
#endif

    // ovr_WaitTillTime(frameTime.NextFrameSeconds - g_ActiveConfig.fTimeWarpTweak);
    Sleep(1);

    ovrLayerEyeFov ld;
    ld.Header.Type = ovrLayerType_EyeFov;
    ld.Header.Flags = (g_ActiveConfig.bFlipVertical ? ovrLayerFlag_TextureOriginAtBottomLeft : 0) |
                      (g_ActiveConfig.bHqDistortion ? ovrLayerFlag_HighQuality : 0);
    for (int eye = 0; eye < 2; eye++)
    {
#if OVR_PRODUCT_VERSION >= 1
      ld.ColorTexture[eye] = pEyeRenderTexture[eye]->TextureChain;
#else
      ld.ColorTexture[eye] = pEyeRenderTexture[eye]->TextureSet;
#endif
      ld.Viewport[eye] = eyeRenderViewport[eye];
      ld.Fov[eye] = g_eye_fov[eye];
      ld.RenderPose[eye] = g_eye_poses[eye];
    }
    ovrLayerHeader* layers = &ld.Header;
    // ovrResult result =
    ovrHmd_SubmitFrame(hmd, 0, nullptr, &layers, 1);

#endif
  }
#endif
}

// New function for stereo rendering submission and timewarp
void VR_RenderFrameStereo()
{
  if (!g_has_hmd || !g_ActiveConfig.bEnableVR)
    return;

  // Assumes eye textures in g_framebuffer_manager have been rendered to by the caller (e.g., D3DGfx::PresentBackbuffer)

  VR_PresentHMDFrame(); // Submits frame to HMD

  // Synchronous Timewarp
  // The logic for g_ActiveConfig.iExtraTimewarpedFrames needs to be set appropriately before this call.
  // This was previously calculated in Hydra's Renderer::SwapImpl based on FPS.
  if (g_ActiveConfig.bSynchronousTimewarp && g_ActiveConfig.iExtraTimewarpedFrames > 0)
  {
    // Ensure VR_GetEyePoses() has been called for the latest head orientation if timewarp needs it.
    // VR_DrawTimewarpFrame in Hydra's Oculus path calls ovrHmd_SubmitFrame with existing poses,
    // but OpenVR might need updated poses for its reprojection if not handled internally by WaitGetPoses.
    // For now, assuming VR_DrawTimewarpFrame handles pose updates or uses last known good ones.
    for (int i = 0; i < g_ActiveConfig.iExtraTimewarpedFrames; ++i)
    {
      VR_DrawTimewarpFrame();
      if (Core::g_drawn_vr < 0xFFFFFFFF) // Prevent overflow if someone leaves it running for ages
          Core::g_drawn_vr++;
    }
  }
  if (Core::g_drawn_vr < 0xFFFFFFFF)
    Core::g_drawn_vr++; // For the main "real" frame submitted
}

}
