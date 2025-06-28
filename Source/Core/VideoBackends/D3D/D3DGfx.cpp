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

namespace DX11
{
Gfx::Gfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale)
    : m_backbuffer_scale(backbuffer_scale), m_swap_chain(std::move(swap_chain))
{
  if (g_ActiveConfig.stereo_mode == StereoMode::OpenVR && Core::g_vr_openvr_instance && Core::g_vr_openvr_instance->IsInitialized())
  {
    m_vrd3d = std::make_unique<VRD3D>(Core::g_vr_openvr_instance.get(), D3D::device.Get());
    if (!m_vrd3d->Init())
    {
      ERROR_LOG_FMT(D3D, "Failed to initialize VRD3D. Disabling VR rendering path.");
      m_vrd3d.reset();
      // NOTE: Ideally, inform the user or fallback g_ActiveConfig.stereo_mode here,
      // but that's complex from this low level.
    }
    else
    {
      INFO_LOG_FMT(D3D, "VRD3D initialized within D3DGfx.");
    }
  }
}

Gfx::~Gfx()
{
  // m_vrd3d will be automatically destroyed by unique_ptr.
  // VRD3D's destructor should handle releasing its own resources.
}

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

  if (m_vrd3d)
  {
    // In VR mode, the main swap chain's framebuffer is not the primary render target initially.
    // Eye-specific render targets will be bound before each eye's render pass.
    // We might still want to clear the swap chain for a companion window.
    if (m_swap_chain)
    {
      // For now, let's clear it. This could be optimized later or used for a companion view.
      SetAndClearFramebuffer(m_swap_chain->GetFramebuffer(), clear_color);
    }
    // Indicate success, but the main "backbuffer" for the game is one of the eye textures.
    return true;
  }

  // Non-VR mode: proceed as usual.
  SetAndClearFramebuffer(m_swap_chain->GetFramebuffer(), clear_color);
  return true;
}

void Gfx::PresentBackbuffer()
{
  if (m_vrd3d)
  {
    // Submit frames to HMD
    if (!m_vrd3d->SubmitFrames())
    {
      ERROR_LOG_FMT(D3D, "Failed to submit frames to OpenVR.");
      // Potentially handle this error, e.g., by stopping VR mode.
    }

    // TODO: Optionally render a companion view to the main swap chain.
    // For now, just present whatever is on the swap chain (likely nothing or a cleared screen).
    // Or, copy one of the eye views to the swap chain.
    // For this initial step, we'll just present.
    if (m_swap_chain)
      m_swap_chain->Present();
  }
  else if (m_swap_chain)
  {
    m_swap_chain->Present();
  }
}

void Gfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

  if (bits & CONFIG_CHANGE_BIT_STEREO_MODE)
  {
    if (g_ActiveConfig.stereo_mode == StereoMode::OpenVR && Core::g_vr_openvr_instance && Core::g_vr_openvr_instance->IsInitialized())
    {
      if (!m_vrd3d) // If not already initialized, or was previously disabled
      {
        INFO_LOG_FMT(D3D, "StereoMode changed to OpenVR. Initializing VRD3D in D3DGfx.");
        m_vrd3d = std::make_unique<VRD3D>(Core::g_vr_openvr_instance.get(), D3D::device.Get());
        if (!m_vrd3d->Init())
        {
          ERROR_LOG_FMT(D3D, "Failed to initialize VRD3D on config change. Disabling VR rendering path.");
          m_vrd3d.reset();
        }
      }
    }
    else
    {
      if (m_vrd3d) // If VR was active but now is not (or VROpenVR failed)
      {
        INFO_LOG_FMT(D3D, "StereoMode changed from OpenVR or OpenVR not available. Shutting down VRD3D in D3DGfx.");
        m_vrd3d.reset(); // This will call VRD3D destructor
      }
    }

    // Quad-buffer changes require swap chain recreation.
    if (m_swap_chain)
        m_swap_chain->SetStereo(SwapChain::WantsStereo());
  }

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

bool Gfx::SetLeftEyeRenderTarget(const ClearColor& clear_color)
{
  if (!m_vrd3d)
  {
    ERROR_LOG_FMT(D3D, "SetLeftEyeRenderTarget called but VRD3D is not initialized.");
    return false;
  }

  DX11::DXTexture* left_eye_texture = m_vrd3d->GetLeftEyeTexture();
  if (!left_eye_texture)
  {
    ERROR_LOG_FMT(D3D, "Failed to get left eye texture from VRD3D.");
    return false;
  }

  // Assuming DXTexture can be wrapped in a DXFramebuffer or used directly as one.
  // For simplicity, let's assume DXTexture itself can be a render target.
  // We need a DXFramebuffer object to use SetAndClearFramebuffer.
  // This implies VRD3D::GetLeftEyeTexture() should return a DXFramebuffer
  // or we need to create one here. Let's adjust VRD3D to return DXFramebuffer later if needed.
  // For now, we assume GetLeftEyeTexture returns something that can be cast or used.
  // This part needs careful review of how DXFramebuffer is created/used.
  // A DXTexture is not directly an AbstractFramebuffer.
  // We need to get/create a framebuffer that USES this texture.
  // For now, let's assume a direct way to set it, this will need fixing.

  DX11::DXFramebuffer* left_fb = m_vrd3d->GetLeftEyeFramebuffer();
  if (!left_fb)
  {
    ERROR_LOG_FMT(D3D, "Failed to get left eye framebuffer from VRD3D.");
    return false;
  }
  SetAndClearFramebuffer(left_fb, clear_color);
  return true;
}

bool Gfx::SetRightEyeRenderTarget(const ClearColor& clear_color)
{
  if (!m_vrd3d)
  {
    ERROR_LOG_FMT(D3D, "SetRightEyeRenderTarget called but VRD3D is not initialized.");
    return false;
  }

  DX11::DXFramebuffer* right_fb = m_vrd3d->GetRightEyeFramebuffer();
  if (!right_fb)
  {
    ERROR_LOG_FMT(D3D, "Failed to get right eye framebuffer from VRD3D.");
    return false;
  }
  SetAndClearFramebuffer(right_fb, clear_color);
  return true;
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
