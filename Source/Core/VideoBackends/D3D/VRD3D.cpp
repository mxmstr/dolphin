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
// #include "VideoBackends/D3D/D3DUtil.h" // For SetDebugObjectName - might need to find alternative
// #include "VideoBackends/D3D/PixelShaderCache.h" // For D3D::drawShadedTexQuad
// #include "VideoBackends/D3D/Render.h" // For D3D::drawShadedTexQuad
// #include "VideoBackends/D3D/VertexShaderCache.h" // For D3D::drawShadedTexQuad

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VR.h"
#include "VideoCommon/VROculus.h"
#include "VideoCommon/VROpenVR.h"


// Forward declaration for g_renderer
namespace DX11
{
class Gfx;
}
extern std::unique_ptr<DX11::Gfx> g_renderer;

// Temporary stand-ins for Gfx members until a proper solution is found
// bool g_is_stereo = false; // To be updated based on g_ActiveConfig.stereo_mode
// int g_eye_count = 1; // To be updated based on g_is_stereo

namespace DX11
{
// Oculus Rift
#ifdef OVR_MAJOR_VERSION

#if OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5
ovrD3D11Texture g_eye_texture[2];
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
    desc.Format = OVR_FORMAT_R8G8B8A8_UNORM_SRGB; // This is an OVR_FORMAT, not DXGI_FORMAT
    desc.Width = size.w;
    desc.Height = size.h;
    desc.MipLevels = 1;
    desc.SampleCount = 1;
    desc.MiscFlags = ovrTextureMisc_DX_Typeless;
    desc.BindFlags = ovrTextureBind_DX_RenderTarget;
    desc.StaticImage = ovrFalse;

    res = ovr_CreateTextureSwapChainDX(hmd0, D3D::device.Get(), &desc, &TextureChain);
    if (OVR_SUCCESS(res))
      ovr_GetTextureSwapChainLength(hmd0, TextureChain, &length);

    if (!OVR_SUCCESS(res))
    {
      ovrErrorInfo e;
      ovr_GetLastErrorInfo(&e);
      PanicAlertFmt(VR,
                    "ovr_CreateTextureSwapChainDX(hmd, OVR_FORMAT_R8G8B8A8_UNORM_SRGB, {}, {})={} "
                    "failed\n{}",
                    size.w, size.h, res, e.ErrorString);
      return;
    }
#elif OVR_MAJOR_VERSION >= 7
    unsigned int miscFlags = ovrSwapTextureSetD3D11_Typeless;
    res = ovr_CreateSwapTextureSetD3D11(hmd0, D3D::device.Get(), &dsDesc, miscFlags, &TextureSet);
    if (OVR_SUCCESS(res))
      length = TextureSet->TextureCount;
    else
       PanicAlertFmt(VR, "ovr_CreateSwapTextureSetD3D11 failed");
#else
    res = ovrHmd_CreateSwapTextureSetD3D11(hmd0, D3D::device.Get(), &dsDesc, &TextureSet);
    if (OVR_SUCCESS(res))
      length = TextureSet->TextureCount;
    else
      PanicAlertFmt(VR, "ovrHmd_CreateSwapTextureSetD3D11 failed");
#endif
    for (int i = 0; i < length; ++i)
    {
#if OVR_PRODUCT_VERSION >= 1
      ID3D11Texture2D* tex = nullptr;
      ovr_GetTextureSwapChainBufferDX(hmd0, TextureChain, i, IID_PPV_ARGS(&tex));
      D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
      rtvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // Oculus examples use UNORM for RTV from SRGB
      rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      ID3D11RenderTargetView* rtv;
      D3D::device->CreateRenderTargetView(tex, &rtvd, &rtv);
      TexRtv.push_back(rtv);
      if (tex)
        tex->Release();
#else
      ovrD3D11Texture* tex = (ovrD3D11Texture*)&TextureSet->Textures[i];
#if OVR_MAJOR_VERSION >= 7
      D3D11_RENDER_TARGET_VIEW_DESC rtvd = {};
      rtvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      D3D::device->CreateRenderTargetView(tex->D3D11.pTexture, &rtvd, &TexRtv[i]);
#else
      D3D::device->CreateRenderTargetView(tex->D3D11.pTexture, nullptr, &TexRtv[i]);
#endif
#endif
    }
  }

#if OVR_PRODUCT_VERSION >= 1
  ID3D11RenderTargetView* GetRTV()
  {
    int index = 0;
    if (hmd && TextureChain) // Add null checks
      ovr_GetTextureSwapChainCurrentIndex(hmd, TextureChain, &index);
    return TexRtv[index];
  }
#endif

  // Commit changes
  void Commit()
  {
#if OVR_PRODUCT_VERSION >= 1
    if (hmd && TextureChain) // Add null checks
      ovr_CommitTextureSwapChain(hmd, TextureChain);
#endif
  }

  void AdvanceToNextTexture()
  {
#if OVR_PRODUCT_VERSION == 0
    if (TextureSet) // Add null check
      TextureSet->CurrentIndex = (TextureSet->CurrentIndex + 1) % TextureSet->TextureCount;
#endif
  }

  void Release(ovrHmd hmd0)
  {
#if OVR_PRODUCT_VERSION >= 1
    for (size_t i = 0; i < TexRtv.size(); ++i)
    {
      if (TexRtv[i])
      {
        TexRtv[i]->Release();
        TexRtv[i] = nullptr;
      }
    }
    TexRtv.clear();
    if (TextureChain)
    {
      ovr_DestroyTextureSwapChain(hmd0, TextureChain);
      TextureChain = nullptr;
    }
#else
    if (TextureSet) // Add null check
      ovrHmd_DestroySwapTextureSet(hmd0, TextureSet);
    TextureSet = nullptr; // Prevent double deletion
#endif
  }
};

OculusTexture* pEyeRenderTexture[2] = {nullptr, nullptr};
ovrRecti eyeRenderViewport[2];
#if OVR_PRODUCT_VERSION >= 1
ovrMirrorTexture mirrorTexture = nullptr;
#else
ovrTexture* mirrorTexture = nullptr;
#endif
int mirror_width = 0, mirror_height = 0;
// D3D11_TEXTURE2D_DESC texdesc = {}; // This was used for mirror texture in older SDKs
#endif

#endif // OVR_MAJOR_VERSION

#ifdef HAVE_OPENVR
// These are defined as globals within this CPP file for OpenVR eye textures
ComPtr<ID3D11Texture2D> m_left_texture = nullptr;
ComPtr<ID3D11Texture2D> m_right_texture = nullptr;
// We need RTVs for these textures to render to them
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
    if (g_has_openvr)
    {
        ID3D11Texture2D* pTexture = nullptr;
        if (eye == 0 && m_left_texture) pTexture = m_left_texture.Get();
        else if (eye == 1 && m_right_texture) pTexture = m_right_texture.Get();

        if (pTexture)
        {
            D3D11_TEXTURE2D_DESC desc;
            pTexture->GetDesc(&desc);
            *width = desc.Width;
            *height = desc.Height;
            return;
        }
    }
#endif
#ifdef OVR_MAJOR_VERSION
    if (g_has_rift && hmd) // Check hmd to be safe
    {
        // For Oculus, dimensions might come from pEyeRenderTexture or eyeRenderViewport
        // This path assumes modern Oculus SDK (>= 0.6) where pEyeRenderTexture is used
        if (pEyeRenderTexture[eye] && eyeRenderViewport[eye].Size.w > 0) {
            *width = eyeRenderViewport[eye].Size.w;
            *height = eyeRenderViewport[eye].Size.h;
            return;
        }
        // Fallback for older SDKs or if above fails
        if (hmdDesc.DefaultEyeFov[eye].UpTan != 0) // Check if hmdDesc is valid
        {
            ovrSizei idealSize = ovr_GetFovTextureSize(hmd, (ovrEyeType)eye, hmdDesc.DefaultEyeFov[eye], 1.0f);
            *width = idealSize.w;
            *height = idealSize.h;
            return;
        }
    }
#endif
    // Fallback if no VR system provided dimensions
    if (*width == 0 || *height == 0) {
        WARN_LOG_FMT(VR, "GetEyeTextureDimensions: Could not determine dimensions for eye {}. Defaulting to EFB size.", eye);
        *width = FramebufferManager::GetEFBWidth();
        *height = FramebufferManager::GetEFBHeight();
    }
}


void VR_ConfigureHMD()
{
#ifdef HAVE_OPENVR
  if (g_has_openvr && m_pCompositor)
  {
    // m_pCompositor->SetGraphicsDevice(vr::Compositor_DeviceType_DirectX, nullptr); // Obsolete
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
    cfg.D3D11.Header.Multisample = 0; // TODO: Use g_ActiveConfig.iMultisamples?
    cfg.D3D11.pDevice = D3D::device.Get();
    cfg.D3D11.pDeviceContext = D3D::context.Get();
    if (g_renderer && g_renderer->GetSwapChain())
      cfg.D3D11.pSwapChain = static_cast<DX11::SwapChain*>(g_renderer->GetSwapChain())->GetDXGISwapChain();
    else
      cfg.D3D11.pSwapChain = nullptr;

    // GetBackBuffer is not available directly, need to go via swapchain framebuffer
    if (g_renderer && g_renderer->GetSwapChain())
    {
       DXFramebuffer* fb = static_cast<DX11::SwapChain*>(g_renderer->GetSwapChain())->GetFramebuffer();
       if (fb && fb->GetRTVArray() && *fb->GetRTVArray())
          cfg.D3D11.pBackBufferRT = *fb->GetRTVArray();
       else
          cfg.D3D11.pBackBufferRT = nullptr;
    }


    if (g_is_direct_mode)
    {
      // D3D::UnloadDXGI(); // D3D namespace doesn't have these directly anymore
      // D3D::LoadDXGI();
      // This was a hack, might not be needed or possible with current DXGI handling
      INFO_LOG_FMT(VR, "Direct Mode AttachToWindow hack might be problematic.");
      // ovrHmd_AttachToWindow(hmd, D3D::hWnd, nullptr, nullptr); // D3D::hWnd also gone
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
    // ... other caps
    ovrHmd_ConfigureRendering(hmd, &cfg.Config, caps, g_eye_fov, g_eye_render_desc);
#if OVR_MAJOR_VERSION <= 4
    ovrhmd_EnableHSWDisplaySDKRender(hmd, false);
#endif
#else // Modern Oculus SDK
    for (int i = 0; i < ovrEye_Count; ++i)
      if (hmd) g_eye_render_desc[i] = ovrHmd_GetRenderDesc(hmd, (ovrEyeType)i, g_eye_fov[i]);
#endif
  }
#endif
}

#if defined(OVR_MAJOR_VERSION) && (OVR_PRODUCT_VERSION >= 1 || OVR_MAJOR_VERSION >= 6)
void RecreateMirrorTextureIfNeeded()
{
  int w = 128;
  int h = 128;
  if (g_renderer && g_renderer->GetSurfaceInfo().swap_chain)
  {
    const D3DCommon::SwapChainInfo& info = static_cast<DX11::SwapChain*>(g_renderer->GetSurfaceInfo().swap_chain)->GetSwapChainInfo();
    w = info.width;
    h = info.height;
  }

  bool bNoMirrorToWindow = g_ActiveConfig.iMirrorPlayer == VR_PLAYER_NONE ||
                           g_ActiveConfig.iMirrorStyle == VR_MIRROR_DISABLED;
  if (w != mirror_width || h != mirror_height || ((mirrorTexture == nullptr) != bNoMirrorToWindow))
  {
    if (mirrorTexture)
    {
      if (hmd) ovr_DestroyMirrorTexture(hmd, mirrorTexture);
      mirrorTexture = nullptr;
    }
    if (!bNoMirrorToWindow && hmd)
    {
#if OVR_PRODUCT_VERSION >= 1
      mirrorTexture = nullptr;
      ovrMirrorTextureDesc desc = {};
      desc.Format = OVR_FORMAT_R8G8B8A8_UNORM; // Typically UNORM for mirror
      desc.Width = w;
      desc.Height = h;
      mirror_width = w;
      mirror_height = h;
      ovrResult result = ovr_CreateMirrorTextureDX(hmd, D3D::device.Get(), &desc, &mirrorTexture);
#else // Older SDKs, texdesc was a global D3D11_TEXTURE2D_DESC
      D3D11_TEXTURE2D_DESC mirror_tex_desc = {};
      mirror_tex_desc.ArraySize = 1;
      mirror_tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
      mirror_tex_desc.Width = w;
      mirror_tex_desc.Height = h;
      mirror_tex_desc.Usage = D3D11_USAGE_DEFAULT;
      mirror_tex_desc.SampleDesc.Count = 1;
      mirror_tex_desc.MipLevels = 1;
      mirror_width = mirror_tex_desc.Width;
      mirror_height = mirror_tex_desc.Height;
      mirrorTexture = nullptr;
#if OVR_MAJOR_VERSION >= 7
      unsigned int miscFlags = ovrSwapTextureSetD3D11_Typeless;
      ovrResult result = ovr_CreateMirrorTextureD3D11(hmd, D3D::device.Get(), &mirror_tex_desc, miscFlags, &mirrorTexture);
#else
      ovrResult result = ovrHmd_CreateMirrorTextureD3D11(hmd, D3D::device.Get(), &mirror_tex_desc, &mirrorTexture);
#endif
#endif
      if (!OVR_SUCCESS(result))
      {
        ERROR_LOG_FMT(VR, "Failed to create D3D mirror texture. Error: {}", result);
        mirrorTexture = nullptr;
      }
    }
  }
}
#endif

void VR_StartFramebuffer()
{
#if defined(OVR_MAJOR_VERSION) && (OVR_PRODUCT_VERSION >= 1 || OVR_MAJOR_VERSION >= 6)
  if (g_has_rift && hmd)
  {
    for (int eye = 0; eye < 2; eye++)
    {
      ovrSizei target_size;
      target_size.w = FramebufferManager::GetEFBWidth(); // Use FramebufferManager for current EFB size
      target_size.h = FramebufferManager::GetEFBHeight();
      if (pEyeRenderTexture[eye]) // Release old one if exists
      {
          pEyeRenderTexture[eye]->Release(hmd);
          delete pEyeRenderTexture[eye];
      }
      pEyeRenderTexture[eye] = new OculusTexture(hmd, target_size);
      eyeRenderViewport[eye].Pos.x = 0;
      eyeRenderViewport[eye].Pos.y = 0;
      eyeRenderViewport[eye].Size = target_size;
    }
    RecreateMirrorTextureIfNeeded();
  }
#endif

#if (defined(OVR_MAJOR_VERSION) && OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5) || \
    defined(HAVE_OPENVR)
  // This block is for older Oculus SDKs (<=0.5) and OpenVR
  // For OpenVR, we create our own textures.
  if ( (g_has_rift && OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5) || g_has_openvr)
  {
    // For OpenVR, we need to create two textures that will be submitted to the compositor.
    // These textures will be used as render targets.
    TextureConfig tex_config = TextureConfig(
        g_framebuffer_manager->GetEFBWidth(), g_framebuffer_manager->GetEFBHeight(), 1, 1,
        g_ActiveConfig.iMultisamples, AbstractTextureFormat::RGBA8,
        AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2D);

    // Create textures using DX11::DXTexture::Create
    // These are the textures OpenVR will use.
    // The old code used FramebufferManager::m_efb.m_frontBuffer which was an array of D3DTexture2D*
    // We will create DXTexture and store their ID3D11Texture2D* pointers in m_left_texture/m_right_texture
    // And also create RTVs for them.

    // For OpenVR, we manage textures explicitly here.
    // FramebufferManager::m_efb.m_frontBuffer is not used in the new backend for this.
    if (g_has_openvr)
    {
      D3D11_TEXTURE2D_DESC tex_desc = {};
      tex_desc.Width = tex_config.width;
      tex_desc.Height = tex_config.height;
      tex_desc.MipLevels = 1;
      tex_desc.ArraySize = 1;
      tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // Common format for VR submission
      tex_desc.SampleDesc.Count = tex_config.samples; // Use MSAA samples from config
      tex_desc.SampleDesc.Quality = 0; // Default quality
      tex_desc.Usage = D3D11_USAGE_DEFAULT;
      tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      tex_desc.CPUAccessFlags = 0;
      tex_desc.MiscFlags = 0;

      HRESULT hr_left = D3D::device->CreateTexture2D(&tex_desc, nullptr, &m_left_texture);
      HRESULT hr_right = D3D::device->CreateTexture2D(&tex_desc, nullptr, &m_right_texture);

      ASSERT_MSG(VR, SUCCEEDED(hr_left) && SUCCEEDED(hr_right), "Failed to create OpenVR eye textures");

      if (SUCCEEDED(hr_left))
      {
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = tex_desc.Format; // Or DXGI_FORMAT_R8G8B8A8_UNORM if SRGB causes issues with RTV
        rtv_desc.ViewDimension = (tex_config.samples > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D::device->CreateRenderTargetView(m_left_texture.Get(), &rtv_desc, &m_left_texture_rtv);
        // D3D::SetDebugObjectName(m_left_texture.Get(), "OpenVR Left Eye Texture");
        // D3D::SetDebugObjectName(m_left_texture_rtv.Get(), "OpenVR Left Eye RTV");

      }
      if (SUCCEEDED(hr_right))
      {
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = tex_desc.Format;
        rtv_desc.ViewDimension = (tex_config.samples > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D::device->CreateRenderTargetView(m_right_texture.Get(), &rtv_desc, &m_right_texture_rtv);
        // D3D::SetDebugObjectName(m_right_texture.Get(), "OpenVR Right Eye Texture");
        // D3D::SetDebugObjectName(m_right_texture_rtv.Get(), "OpenVR Right Eye RTV");
      }
    }
#if defined(OVR_MAJOR_VERSION) && OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5
    // Legacy Oculus SDK 0.5 path (similar to OpenVR, but uses FramebufferManager::m_efb.m_frontBuffer)
    if (g_has_rift)
    {
        // This path created D3DTexture2D objects and stored them in FramebufferManager::m_efb.m_frontBuffer
        // This structure is no longer available or suitable. This path needs significant rework
        // or removal if SDK 0.5 is not a target. For now, commenting out the problematic parts.
        /*
        ID3D11Texture2D* buf;
        DXGI_SAMPLE_DESC sample_desc;
        sample_desc.Count = g_ActiveConfig.iMultisamples;
        sample_desc.Quality = 0;
        D3D11_TEXTURE2D_DESC texdesc0 = CD3D11_TEXTURE2D_DESC(
            DXGI_FORMAT_R8G8B8A8_UNORM, FramebufferManager::GetEFBWidth(),
            FramebufferManager::GetEFBHeight(), 1, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
            D3D11_USAGE_DEFAULT, 0, sample_desc.Count, sample_desc.Quality); // sample_desc.Quality was 1 before, should be 0 for count > 0

        for (int eye = 0; eye < 2; ++eye)
        {
            HRESULT hr = D3D::device->CreateTexture2D(&texdesc0, nullptr, &buf);
            ASSERT_MSG(VR, hr == S_OK, "create Oculus Rift eye texture (size: %dx%d; hr=%#x)",
                    FramebufferManager::GetEFBWidth(), FramebufferManager::GetEFBHeight(), hr);

            // FramebufferManager::m_efb.m_frontBuffer[eye] = new D3DTexture2D(...); // This is the old class
            // This needs to be replaced with DXTexture::Create or similar and stored appropriately
            // For SDK 0.5, g_eye_texture[eye].D3D11.pTexture was set to this.
            if (buf) buf->Release(); // D3DTexture2D would take ownership
        }
        // ... SetDebugObjectName calls ...
        // ... g_eye_texture setup ...
        */
        INFO_LOG_FMT(VR, "Oculus SDK 0.5 path in VR_StartFramebuffer needs rework.");
    }
#endif
  }
#endif
}

void VR_StopFramebuffer()
{
#if defined(OVR_MAJOR_VERSION) && (OVR_PRODUCT_VERSION >= 1 || OVR_MAJOR_VERSION >= 6)
  if (mirrorTexture)
  {
    if (hmd) ovr_DestroyMirrorTexture(hmd, mirrorTexture);
    mirrorTexture = nullptr;
  }
  for (int eye = 0; eye < 2; eye++)
  {
    if (pEyeRenderTexture[eye])
    {
      if (hmd) pEyeRenderTexture[eye]->Release(hmd);
      delete pEyeRenderTexture[eye];
      pEyeRenderTexture[eye] = nullptr;
    }
  }
#endif
#if defined(OVR_MAJOR_VERSION) && OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5
  // if (g_has_rift)
  // {
    // SAFE_RELEASE on FramebufferManager::m_efb.m_frontBuffer is not applicable.
    // These textures are managed by g_eye_texture or need manual release if raw pointers were stored.
  // }
#endif
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
#ifdef OVR_MAJOR_VERSION
  if (g_has_rift && hmd)
  {
#if OVR_PRODUCT_VERSION >= 1 || OVR_MAJOR_VERSION >= 6
    RecreateMirrorTextureIfNeeded();
    ++g_ovr_frameindex;
#if OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 7 // SDK 0.6, 0.7
    g_rift_frame_timing = ovrHmd_GetFrameTiming(hmd, 0);
#endif
    for (int eye = 0; eye < 2; eye++)
    {
      if (pEyeRenderTexture[eye])
        pEyeRenderTexture[eye]->AdvanceToNextTexture();
    }
#else // SDK 0.5 or earlier
    ovrHmd_DismissHSWDisplay(hmd);
    g_rift_frame_timing = ovrHmd_BeginFrame(hmd, ++g_ovr_frameindex);
#endif
  }
#endif
}

void VR_RenderToEyebuffer(int eye, int hmd_number)
{
#ifdef OVR_MAJOR_VERSION
  if (g_has_rift && hmd && (hmd_number == 1 || !g_has_openvr)) // Oculus rendering
  {
#if OVR_PRODUCT_VERSION >= 1
    if (pEyeRenderTexture[eye])
    {
      ID3D11RenderTargetView* rtv = pEyeRenderTexture[eye]->GetRTV();
      D3D::context->OMSetRenderTargets(1, &rtv, nullptr);
    }
#elif OVR_MAJOR_VERSION >= 6 // SDK 0.6 to 0.8
    if (pEyeRenderTexture[eye] && pEyeRenderTexture[eye]->TextureSet)
    {
        D3D::context->OMSetRenderTargets(
            1, &pEyeRenderTexture[eye]->TexRtv[pEyeRenderTexture[eye]->TextureSet->CurrentIndex],
            nullptr);
    }
#else // SDK 0.5 or earlier
    // This path used FramebufferManager::m_efb.m_frontBuffer[eye]->GetRTV()
    // which needs to be re-evaluated if SDK 0.5 is supported.
    // For now, this path is problematic.
    INFO_LOG_FMT(VR, "Oculus SDK 0.5 RenderToEyebuffer path needs rework.");
#endif
  }
#endif
#if defined(HAVE_OPENVR)
  if (g_has_openvr && (hmd_number == 0 || !g_has_rift)) // OpenVR rendering
  {
    ID3D11RenderTargetView* rtv = (eye == 0) ? m_left_texture_rtv.Get() : m_right_texture_rtv.Get();
    if (rtv)
    {
      D3D::context->OMSetRenderTargets(1, &rtv, nullptr);
    }
    else
    {
      // Fallback or error: try to use EFB? This indicates eye textures weren't set up.
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
#ifdef HAVE_OPENVR
  if (g_has_openvr && m_pCompositor && m_left_texture && m_right_texture)
  {
    vr::Texture_t leftEyeTexture = {m_left_texture.Get(), vr::TextureType_DirectX, vr::ColorSpace_Gamma};
    vr::VRCompositor()->Submit(vr::Eye_Left, &leftEyeTexture);
    vr::Texture_t rightEyeTexture = {m_right_texture.Get(), vr::TextureType_DirectX, vr::ColorSpace_Gamma};
    vr::VRCompositor()->Submit(vr::Eye_Right, &rightEyeTexture);

    // WaitGetPoses called in VR_UpdateTracking
    // m_pCompositor->WaitGetPoses(m_rTrackedDevicePose, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    // g_older_tracking_time = g_old_tracking_time;
    // g_old_tracking_time = g_last_tracking_time;
    // g_last_tracking_time = Common::Timer::NowMs() / 1000.0; // Timer::NowMs might not exist

    if (g_ActiveConfig.iMirrorStyle != VR_MIRROR_DISABLED &&
        g_ActiveConfig.iMirrorPlayer != VR_PLAYER_NONE && g_renderer && g_renderer->GetSwapChain())
    {
      DX11::SwapChain* swap_chain = static_cast<DX11::SwapChain*>(g_renderer->GetSwapChain());
      AbstractTexture* efb_resolved_tex = g_framebuffer_manager->GetEFBColorTexture(); // Or ResolveEFBColorTexture if MSAA

      if (efb_resolved_tex) // Ensure EFB texture is valid
      {
          MathUtil::Rectangle<int> sourceRc(0,0, efb_resolved_tex->GetWidth(), efb_resolved_tex->GetHeight());
          DXTexture* dx_efb_tex = static_cast<DXTexture*>(efb_resolved_tex);

          // Bind main backbuffer
          DXFramebuffer* backbuffer_fb = swap_chain->GetFramebuffer();
          if(backbuffer_fb)
            D3D::context->OMSetRenderTargets(backbuffer_fb->GetNumRTVs(), backbuffer_fb->GetRTVArray(), backbuffer_fb->GetDSV());

          D3D11_VIEWPORT vp = CD3D11_VIEWPORT(0.0f, 0.0f, (float)swap_chain->GetWidth(), (float)swap_chain->GetHeight());

          int eye_to_draw = 0; // Default to left eye or mono
          if (g_ActiveConfig.iMirrorStyle >= VR_MIRROR_WARPED) // Both eyes
             vp.Width *= 0.5f;
          else // Single eye
             eye_to_draw = (g_ActiveConfig.iMirrorStyle == VR_MIRROR_RIGHT) ? 1 : 0;

          D3D::context->RSSetViewports(1, &vp);

          ID3D11ShaderResourceView* srv_to_draw = (eye_to_draw == 0) ? static_cast<DXTexture*>(g_framebuffer_manager->GetEFBColorTexture())->GetD3DSRV() : static_cast<DXTexture*>(g_framebuffer_manager->GetEFBColorTexture())->GetD3DSRV(); // Placeholder for actual eye SRVs
          // This needs to use the SRV from m_left_texture or m_right_texture if mirroring those
          // For now, let's assume we are mirroring the EFB content, which might be one of the eyes already.
          // If we want to mirror the submitted textures:
          // srv_to_draw = (eye_to_draw == 0) ? m_left_texture_srv.Get() : m_right_texture_srv.Get();
          // But m_left_texture doesn't have an SRV created by default.
          // For now, using EFB as source for mirror.

          // D3D::drawShadedTexQuad is not directly available.
          // Need to use appropriate shaders and drawing logic from the current renderer.
          // This is a placeholder for the actual drawing call.
          // PixelShaderManager::GetInstance()->SetPixelShader(PixelShaderUid::PASSTHROUGH_COLOR);
          // VertexShaderManager::GetInstance()->SetVertexShader(VertexShaderUid::POS_UV);
          // g_renderer->Draw(); // This is too generic
          INFO_LOG_FMT(VR, "VR Mirror mode drawing (OpenVR) needs to be implemented with current rendering system. Commenting out for now.");
          // TODO: Implement actual drawing for mirror mode here.
          // Example:
          // g_renderer->SetTexture(0, dx_efb_tex); // Or the correct eye texture
          // g_renderer->SetSamplerState(0, {}); // Default sampler
          // Setup shaders for textured quad
          // g_renderer->Draw(...);


          if (g_ActiveConfig.iMirrorStyle >= VR_MIRROR_WARPED)
          {
            vp.TopLeftX += vp.Width;
            D3D::context->RSSetViewports(1, &vp);
            // Draw right eye texture (or EFB again, if that's the source)
            // srv_to_draw = m_right_texture_srv.Get(); or EFB SRV
            INFO_LOG_FMT(VR, "VR Mirror mode drawing for second eye (OpenVR) needs implementation. Commenting out for now.");
            // TODO: Implement actual drawing for second eye mirror mode here.
          }
          swap_chain->Present();
      }
    }
  }
#endif
#ifdef OVR_MAJOR_VERSION
  if (g_has_rift && hmd)
  {
#if OVR_PRODUCT_VERSION >= 1
    if(pEyeRenderTexture[0]) pEyeRenderTexture[0]->Commit();
    if(pEyeRenderTexture[1]) pEyeRenderTexture[1]->Commit();
#endif

    ComPtr<ID3D11BlendState> pOculusRiftBlendState = nullptr;
    D3D11_BLEND_DESC oculusBlendDesc;
    ZeroMemory(&oculusBlendDesc, sizeof(D3D11_BLEND_DESC));
    oculusBlendDesc.AlphaToCoverageEnable = FALSE;
    oculusBlendDesc.IndependentBlendEnable = FALSE; // FALSE if all RTs same, TRUE if different
    oculusBlendDesc.RenderTarget[0].BlendEnable = FALSE;
    oculusBlendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr = D3D::device->CreateBlendState(&oculusBlendDesc, &pOculusRiftBlendState);
    if (FAILED(hr))
      PanicAlertFmt("Failed to create blend state at {} {}\n", __FILE__, __LINE__);
    // D3D::SetDebugObjectName(pOculusRiftBlendState.Get(), "blend state used to make sure rift draw call works");

    D3D::context->OMSetBlendState(pOculusRiftBlendState.Get(), nullptr, 0xFFFFFFFF);

#if OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5
    // ovrHmd_EndFrame(hmd, g_eye_poses, &g_eye_texture[0].Texture); // g_eye_texture is problematic
    INFO_LOG_FMT(VR, "Oculus SDK 0.5 EndFrame needs rework for g_eye_texture.");
#else // Modern Oculus SDK
    ovrLayerEyeFov ld;
    ld.Header.Type = ovrLayerType_EyeFov;
    ld.Header.Flags = (g_ActiveConfig.bFlipVertical ? ovrLayerFlag_TextureOriginAtBottomLeft : 0) |
                      (g_ActiveConfig.bHqDistortion ? ovrLayerFlag_HighQuality : 0);
    for (int eye = 0; eye < 2; eye++)
    {
      if (pEyeRenderTexture[eye])
      {
#if OVR_PRODUCT_VERSION >= 1
        ld.ColorTexture[eye] = pEyeRenderTexture[eye]->TextureChain;
#else
        ld.ColorTexture[eye] = pEyeRenderTexture[eye]->TextureSet;
#endif
        ld.Viewport[eye] = eyeRenderViewport[eye];
        ld.Fov[eye] = g_eye_fov[eye];
        ld.RenderPose[eye] = g_eye_poses[eye]; // g_eye_poses should be updated by VR_UpdateTracking
      }
      else // Provide a null texture if one isn't ready (black screen for that eye)
      {
         ld.ColorTexture[eye] = nullptr;
      }
    }
    ovrLayerHeader* layers = &ld.Header;
    ovrHmd_SubmitFrame(hmd, 0, nullptr, &layers, 1);

    if (mirrorTexture && g_ActiveConfig.iMirrorPlayer != VR_PLAYER_NONE &&
        g_ActiveConfig.iMirrorStyle != VR_MIRROR_DISABLED && g_renderer && g_renderer->GetSurfaceInfo().swap_chain)
    {
      DX11::SwapChain* swap_chain = static_cast<DX11::SwapChain*>(g_renderer->GetSurfaceInfo().swap_chain);
      DXFramebuffer* backbuffer_fb = swap_chain->GetFramebuffer();
      ComPtr<ID3D11Texture2D> backbuffer_tex_com;

      if (backbuffer_fb && backbuffer_fb->GetColorAttachment())
         backbuffer_tex_com = static_cast<DXTexture*>(backbuffer_fb->GetColorAttachment())->GetD3DTexture();


      if (g_ActiveConfig.iMirrorStyle == VR_MIRROR_WARPED)
      {
#if OVR_PRODUCT_VERSION >= 1
        ComPtr<ID3D11Texture2D> tex = nullptr;
        if (hmd) ovr_GetMirrorTextureBufferDX(hmd, mirrorTexture, IID_PPV_ARGS(&tex));
        if (tex && backbuffer_tex_com)
        {
          D3D::context->CopyResource(backbuffer_tex_com.Get(), tex.Get());
        }
#else // Older SDKs
        // ovrD3D11Texture* tex = (ovrD3D11Texture*)mirrorTexture;
        // ... drawShadedTexQuad logic ...
        INFO_LOG_FMT(VR, "Oculus SDK < 1.0 mirror warped mode needs draw logic.");
#endif
      }
      else // Mirror single eye (non-warped)
      {
        // This also needs drawShadedTexQuad or equivalent
        INFO_LOG_FMT(VR, "Oculus mirror single eye mode needs draw logic.");
      }
      swap_chain->Present();
    }
#endif
  }
#endif
}

void VR_DrawTimewarpFrame() // This function seems specific to older Oculus SDKs for sync timewarp
{
#ifdef OVR_MAJOR_VERSION
  if (g_has_rift && hmd)
  {
#if OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 5
    // ovrFrameTiming frameTime = ovrHmd_BeginFrame(hmd, ++g_ovr_frameindex);
    // ovr_WaitTillTime(frameTime.NextFrameSeconds - g_ActiveConfig.fTimeWarpTweak);
    // ovrHmd_EndFrame(hmd, g_eye_poses, &g_eye_texture[0].Texture);
    INFO_LOG_FMT(VR, "Oculus SDK 0.5 VR_DrawTimewarpFrame needs rework for g_eye_texture.");
#else // Modern Oculus SDK handles timewarp, but this function might be for custom sync timewarp
    ++g_ovr_frameindex;
#if OVR_PRODUCT_VERSION == 0 && OVR_MAJOR_VERSION <= 7 // SDK 0.6, 0.7
    // ovrFrameTiming frameTime = ovrHmd_GetFrameTiming(hmd, 0);
#endif
    // Sleep(1); // Not a good way to sync

    ovrLayerEyeFov ld;
    ld.Header.Type = ovrLayerType_EyeFov;
    ld.Header.Flags = (g_ActiveConfig.bFlipVertical ? ovrLayerFlag_TextureOriginAtBottomLeft : 0) |
                      (g_ActiveConfig.bHqDistortion ? ovrLayerFlag_HighQuality : 0);
    for (int eye = 0; eye < 2; eye++)
    {
       if (pEyeRenderTexture[eye])
       {
#if OVR_PRODUCT_VERSION >= 1
          ld.ColorTexture[eye] = pEyeRenderTexture[eye]->TextureChain;
#else
          ld.ColorTexture[eye] = pEyeRenderTexture[eye]->TextureSet;
#endif
          ld.Viewport[eye] = eyeRenderViewport[eye];
          ld.Fov[eye] = g_eye_fov[eye];
          ld.RenderPose[eye] = g_eye_poses[eye]; // Pose should be most recent
       }
       else
       {
          ld.ColorTexture[eye] = nullptr;
       }
    }
    ovrLayerHeader* layers = &ld.Header;
    ovrHmd_SubmitFrame(hmd, 0, nullptr, &layers, 1); // Re-submit with latest pose
#endif
  }
#endif
}
}
