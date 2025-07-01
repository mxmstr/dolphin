// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoBackends/D3D/D3DGfx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <strsafe.h>
#include <tuple>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"

#include "Core/Core.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DBoundingBox.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DSwapChain.h"
#include "VideoBackends/D3D/DXPipeline.h"
#include "VideoBackends/D3D/DXShader.h"
#include "VideoBackends/D3D/DXTexture.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/VR.h"                 // For VR state (g_has_hmd, etc.)
#include "VideoBackends/D3D/VRD3D.h"        // For DX11::VR_... functions
#include "VideoCommon/ShaderCache.h"        // For g_shader_cache
#include "VideoCommon/OnScreenDisplay.h"    // For OSD::DrawMessages
#include "VideoCommon/Renderer.h"           // For g_renderer (though access might be AbstractGfx)

// For D3D::drawShadedTexQuad equivalent or pipeline setup
#include "VideoBackends/D3D/D3DState.h"


namespace DX11
{
Gfx::Gfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale)
    : m_backbuffer_scale(backbuffer_scale), m_swap_chain(std::move(swap_chain))
{
}

Gfx::~Gfx() = default;

bool Gfx::IsHeadless() const
{
  return !m_swap_chain;
}

std::unique_ptr<AbstractTexture> Gfx::CreateTexture(const TextureConfig& config,
                                                    std::string_view name)
{
  return DXTexture::Create(config, name);
}

std::unique_ptr<AbstractStagingTexture> Gfx::CreateStagingTexture(StagingTextureType type,
                                                                  const TextureConfig& config)
{
  return DXStagingTexture::Create(type, config);
}

std::unique_ptr<AbstractFramebuffer>
Gfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                       std::vector<AbstractTexture*> additional_color_attachments)
{
  return DXFramebuffer::Create(static_cast<DXTexture*>(color_attachment),
                               static_cast<DXTexture*>(depth_attachment),
                               std::move(additional_color_attachments));
}

std::unique_ptr<AbstractShader>
Gfx::CreateShaderFromSource(ShaderStage stage, std::string_view source, std::string_view name)
{
  auto bytecode = DXShader::CompileShader(D3D::feature_level, stage, source);
  if (!bytecode)
    return nullptr;

  return DXShader::CreateFromBytecode(stage, std::move(*bytecode), name);
}

std::unique_ptr<AbstractShader> Gfx::CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                            size_t length, std::string_view name)
{
  return DXShader::CreateFromBytecode(stage, DXShader::CreateByteCode(data, length), name);
}

std::unique_ptr<AbstractPipeline> Gfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                      const void* cache_data,
                                                      size_t cache_data_length)
{
  return DXPipeline::Create(config);
}

void Gfx::SetPipeline(const AbstractPipeline* pipeline)
{
  const DXPipeline* dx_pipeline = static_cast<const DXPipeline*>(pipeline);
  if (m_current_pipeline == dx_pipeline)
    return;

  if (dx_pipeline)
  {
    D3D::stateman->SetRasterizerState(dx_pipeline->GetRasterizerState());
    D3D::stateman->SetDepthState(dx_pipeline->GetDepthState());
    D3D::stateman->SetBlendState(dx_pipeline->GetBlendState());
    D3D::stateman->SetPrimitiveTopology(dx_pipeline->GetPrimitiveTopology());
    D3D::stateman->SetInputLayout(dx_pipeline->GetInputLayout());
    D3D::stateman->SetVertexShader(dx_pipeline->GetVertexShader());
    D3D::stateman->SetGeometryShader(dx_pipeline->GetGeometryShader());
    D3D::stateman->SetPixelShader(dx_pipeline->GetPixelShader());
    D3D::stateman->SetIntegerRTV(dx_pipeline->UseLogicOp());
  }
  else
  {
    // These will be destroyed at pipeline destruction.
    D3D::stateman->SetInputLayout(nullptr);
    D3D::stateman->SetVertexShader(nullptr);
    D3D::stateman->SetGeometryShader(nullptr);
    D3D::stateman->SetPixelShader(nullptr);
  }
}

void Gfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  // TODO: Move to stateman
  const CD3D11_RECT rect(rc.left, rc.top, std::max(rc.right, rc.left + 1),
                         std::max(rc.bottom, rc.top + 1));
  D3D::context->RSSetScissorRects(1, &rect);
}

void Gfx::SetViewport(float x, float y, float width, float height, float near_depth,
                      float far_depth)
{
  // TODO: Move to stateman
  const CD3D11_VIEWPORT vp(x, y, width, height, near_depth, far_depth);
  D3D::context->RSSetViewports(1, &vp);
}

void Gfx::Draw(u32 base_vertex, u32 num_vertices)
{
  D3D::stateman->Apply();
  D3D::context->Draw(num_vertices, base_vertex);
}

void Gfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  D3D::stateman->Apply();
  D3D::context->DrawIndexed(num_indices, base_index, base_vertex);
}

void Gfx::DispatchComputeShader(const AbstractShader* shader, u32 groupsize_x, u32 groupsize_y,
                                u32 groupsize_z, u32 groups_x, u32 groups_y, u32 groups_z)
{
  D3D::stateman->SetComputeShader(static_cast<const DXShader*>(shader)->GetD3DComputeShader());
  D3D::stateman->SyncComputeBindings();
  D3D::context->Dispatch(groups_x, groups_y, groups_z);
}

bool Gfx::BindBackbuffer(const ClearColor& clear_color)
{
  CheckForSwapChainChanges();
  SetAndClearFramebuffer(m_swap_chain->GetFramebuffer(), clear_color);
  return true;
}

void Gfx::PresentBackbuffer()
{
  if (g_has_hmd && g_ActiveConfig.bEnableVR && g_framebuffer_manager && g_shader_cache && m_swap_chain)
  {
    // VR Rendering Path
    // TODO: Manage g_first_rift_frame state or equivalent for VR_BeginFrame/VR_GetEyePoses calls
    // This might need to be tied to the game's frame presentation rather than every PresentBackbuffer call,
    // especially with timewarp. For now, call it once.
    // A proper solution would involve a "new real frame" flag.
    static bool vr_frame_initialized = false;
    if (!vr_frame_initialized) {
        DX11::VR_BeginFrame(); // Specific to some VR SDKs, might need abstraction or conditional call
        vr_frame_initialized = true; // Simplistic, needs better handling for ongoing frames
    }
    DX11::VR_GetEyePoses();


    const int eye_count = g_framebuffer_manager->GetEyeCount();
    const u32 target_width = g_framebuffer_manager->GetEFBWidth();
    const u32 target_height = g_framebuffer_manager->GetEFBHeight();
    MathUtil::Rectangle<int> efb_region(0, 0, target_width, target_height);

    // Resolve EFB once before looping for eyes
    // The region for ResolveEFBColorTexture should be the full EFB if we're copying the whole thing.
    VideoCommon::AbstractTexture* resolved_efb_color_texture =
        g_framebuffer_manager->ResolveEFBColorTexture(efb_region);

    if (!resolved_efb_color_texture)
    {
      ERROR_LOG(VR, "Failed to resolve EFB color texture for VR presentation.");
      m_swap_chain->Present(); // Fallback to normal presentation
      return;
    }
    DXTexture* dx_resolved_efb_texture = static_cast<DXTexture*>(resolved_efb_color_texture);

    for (int eye = 0; eye < eye_count; ++eye)
    {
      DX11::VR_RenderToEyebuffer(eye); // Sets render target to g_framebuffer_manager->m_vr_eye_textures[eye]

      // Set viewport for the current eye
      SetViewport(0.0f, 0.0f, static_cast<float>(target_width), static_cast<float>(target_height), 0.0f, 1.0f);
      // Scissor default matches viewport, or set explicitly if needed
      SetScissorRect(MathUtil::Rectangle<int>(0, 0, target_width, target_height));

      // Blit from resolved_efb_color_texture to the current eye's render target
      const VideoCommon::AbstractShader* abstract_vs = g_shader_cache->GetTextureCopyVertexShader();
      const VideoCommon::AbstractShader* abstract_ps = nullptr;
      const VideoCommon::AbstractShader* abstract_gs = nullptr;

      if (g_ActiveConfig.iStereoMode == StereoMode::OSVR) // Assuming STEREO_OSVR is defined in StereoMode enum
      {
        abstract_ps = g_shader_cache->GetOSVRPixelShader();
        // OSVR shader might need geometry shader if uv0.z (eye index) is passed via instance ID
        if (g_shader_cache->GetTexcoordGeometryShader()) // Check if GS is available/needed
             abstract_gs = g_shader_cache->GetTexcoordGeometryShader();
      }
      else
      {
        abstract_ps = g_shader_cache->GetTextureCopyPixelShader();
        // If rendering to texture array slices, GS would be used.
        // But we render to separate eye textures, so GS for layering might not be needed here.
        // If GetVREyeTexture(eye) returns a slice of an array, then GS is needed.
        // Based on FramebufferManager changes, m_vr_eye_textures are individual Texture_2D.
      }

      if (abstract_vs && abstract_ps)
      {
        // Simplified pipeline setup for blit
        // A full AbstractPipeline would be better
        D3D::stateman->SetVertexShader(static_cast<const DXShader*>(abstract_vs)->GetD3DVertexShader());
        D3D::stateman->SetPixelShader(static_cast<const DXShader*>(abstract_ps)->GetD3DPixelShader());
        if (abstract_gs)
            D3D::stateman->SetGeometryShader(static_cast<const DXShader*>(abstract_gs)->GetD3DGeometryShader());
        else
            D3D::stateman->SetGeometryShader(nullptr);

        // Input Assembler (IA)
        D3D::stateman->SetInputLayout(nullptr); // For screen-quad shaders, often no specific IA needed or handled by VS
        D3D::context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Rasterizer State (RS) - default should be fine for fullscreen quad
        // DepthStencil State (DS) - typically no depth test/write for this blit
        D3D::stateman->SetDepthState(D3D::RenderState::GetNoDepthTestingDepthState());
        // Blend State (BS) - no blending
        D3D::stateman->SetBlendState(D3D::RenderState::GetNoBlendingBlendState());

        SetTexture(0, dx_resolved_efb_texture);
        SetSamplerState(0, D3D::RenderState::GetLinearSamplerState());

        D3D::stateman->Apply();
        Draw(0, 3); // Draw a single triangle that covers the screen (requires specific VS)
                      // Or use a utility to draw a screen quad.
                      // g_vertex_manager->DrawScreenQuad(); // This would be ideal
      }
      else
      {
        ERROR_LOG(VR, "Failed to get shaders for VR eye blit for eye %d.", eye);
      }

      // TODO: Render OSD/Debug text per eye if necessary
      // OSD::DrawMessages(); // This would draw to the current RT (eye texture)
    }

    // After both eyes are rendered:
    OSD::DrawMessages(); // Draw OSD to the EFB (if it's not per-eye) or to a specific layer if needed.
                         // If OSD should be in VR, it needs to be rendered to each eye texture or EFB before resolve.
                         // For now, assuming OSD is part of the EFB content passed in.

    DX11::VR_RenderFrameStereo(); // Handles HMD submission and sync timewarp

    // Desktop mirror presentation (if not handled by VR_PresentHMDFrame already)
    // Some VR SDKs (like Oculus) might handle mirror via their submission.
    // OpenVR in Hydra handled its own mirror in VR_PresentHMDFrame.
    // If m_swap_chain is the main window, this presents to it.
    // Check if VR_PresentHMDFrame already presented to the screen.
    // For now, assume VR_PresentHMDFrame handles mirror if needed, or this presents a non-VR view.
    if(g_ActiveConfig.iMirrorPlayer != VR_PLAYER_NONE && g_ActiveConfig.iMirrorStyle != VR_MIRROR_DISABLED) {
        // If VR_PresentHMDFrame in VRD3D.cpp handles its own mirror (like Hydra's OpenVR path did),
        // then this m_swap_chain->Present() might be redundant or show something else.
        // If VR_PresentHMDFrame *doesn't* handle the mirror, this is where it would happen,
        // potentially blitting one of the eye textures or a composite to m_swap_chain.
        // For now, let's assume the mirror is handled by VR_PresentHMDFrame.
        // If not, the following Present() will just show whatever was last in the backbuffer.
    }
     m_swap_chain->Present(); // This ensures the main window updates, could be a black screen or game view depending on VR SDK
  }
  else
  {
    // Original non-VR presentation logic
    m_swap_chain->Present();
  }
}

void Gfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

  // Quad-buffer changes require swap chain recreation.
  if (bits & CONFIG_CHANGE_BIT_STEREO_MODE && m_swap_chain)
    m_swap_chain->SetStereo(SwapChain::WantsStereo());

  if (bits & CONFIG_CHANGE_BIT_HDR && m_swap_chain)
    m_swap_chain->SetHDR(SwapChain::WantsHDR());
}

void Gfx::CheckForSwapChainChanges()
{
  const bool surface_changed = g_presenter->SurfaceChangedTestAndClear();
  const bool surface_resized =
      g_presenter->SurfaceResizedTestAndClear() || m_swap_chain->CheckForFullscreenChange();
  if (!surface_changed && !surface_resized)
    return;

  if (surface_changed)
  {
    m_swap_chain->ChangeSurface(g_presenter->GetNewSurfaceHandle());
  }
  else
  {
    m_swap_chain->ResizeSwapChain();
  }

  g_presenter->SetBackbuffer(m_swap_chain->GetWidth(), m_swap_chain->GetHeight());
}

void Gfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;

  // We can't leave the framebuffer bound as a texture and a render target.
  DXFramebuffer* fb = static_cast<DXFramebuffer*>(framebuffer);
  fb->Unbind();

  D3D::stateman->SetFramebuffer(fb);
  m_current_framebuffer = fb;
}

void Gfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  SetFramebuffer(framebuffer);
}

void Gfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                 float depth_value)
{
  SetFramebuffer(framebuffer);
  D3D::stateman->Apply();

  DXFramebuffer* fb = static_cast<DXFramebuffer*>(framebuffer);
  fb->Clear(color_value, depth_value);
}

void Gfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  D3D::stateman->SetTexture(index, texture ? static_cast<const DXTexture*>(texture)->GetD3DSRV() :
                                             nullptr);
}

void Gfx::SetSamplerState(u32 index, const SamplerState& state)
{
  D3D::stateman->SetSampler(index, m_state_cache.Get(state));
}

void Gfx::SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write)
{
  D3D::stateman->SetComputeUAV(index,
                               texture ? static_cast<DXTexture*>(texture)->GetD3DUAV() : nullptr);
}

void Gfx::UnbindTexture(const AbstractTexture* texture)
{
  if (D3D::stateman->UnsetTexture(static_cast<const DXTexture*>(texture)->GetD3DSRV()) != 0)
    D3D::stateman->ApplyTextures();
}

void Gfx::Flush()
{
  D3D::context->Flush();
}

void Gfx::WaitForGPUIdle()
{
  // There is no glFinish() equivalent in D3D.
  D3D::context->Flush();
}

void Gfx::SetFullscreen(bool enable_fullscreen)
{
  if (m_swap_chain)
    m_swap_chain->SetFullscreen(enable_fullscreen);
}

bool Gfx::IsFullscreen() const
{
  return m_swap_chain && m_swap_chain->GetFullscreen();
}

SurfaceInfo Gfx::GetSurfaceInfo() const
{
  return {m_swap_chain ? static_cast<u32>(m_swap_chain->GetWidth()) : 0,
          m_swap_chain ? static_cast<u32>(m_swap_chain->GetHeight()) : 0, m_backbuffer_scale,
          m_swap_chain ? m_swap_chain->GetFormat() : AbstractTextureFormat::Undefined};
}

}  // namespace DX11
