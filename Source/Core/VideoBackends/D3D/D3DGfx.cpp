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
#include "VideoCommon/VROpenVR.h"
#include "VideoCommon/VideoBackendBase.h"

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
  // VR HMD Submission
  if (g_video_backend && g_video_backend->GetVROpenVR() &&
      g_video_backend->GetVROpenVR()->IsInitialized() && g_ActiveConfig.stereo_mode == StereoMode::OpenVR)
  {
    VROpenVR* vr_system = g_video_backend->GetVROpenVR();
    DXTexture* main_swapchain_dxtexture = m_swap_chain->GetTexture();

    if (main_swapchain_dxtexture && main_swapchain_dxtexture->GetD3DTexture())
    {
      ID3D11Texture2D* d3d_main_texture = static_cast<ID3D11Texture2D*>(main_swapchain_dxtexture->GetD3DTexture());
      D3D11_TEXTURE2D_DESC main_desc;
      d3d_main_texture->GetDesc(&main_desc);

      uint32_t rec_width = 0, rec_height = 0;
      if (!vr_system->GetRecommendedRenderTargetSize(&rec_width, &rec_height) || rec_width == 0 || rec_height == 0)
      {
        ERROR_LOG_FMT(VR, "Could not get recommended VR render target size for submission.");
        // Attempt to submit the full texture as a fallback (will be wrong)
        if (!vr_system->SubmitFrames(d3d_main_texture, d3d_main_texture))
        {
          ERROR_LOG_FMT(VR, "Fallback HMD frame submission failed.");
        }
      }
      else
      {
        // Ensure our separate eye textures are ready and correctly sized
        EnsureVREyeTextures(rec_width, rec_height);

        if (!m_vr_eye_texture_left || !m_vr_eye_texture_right)
        {
          ERROR_LOG_FMT(VR, "VR eye textures are not created for submission.");
        }
        else
        {
          // Define source regions for copying from the double-wide backbuffer
          D3D11_BOX source_box_left = {};
          source_box_left.left = 0;
          source_box_left.top = 0;
          source_box_left.front = 0;
          source_box_left.right = rec_width;
          source_box_left.bottom = rec_height;
          source_box_left.back = 1; // For Texture2D, depth is 1

          D3D11_BOX source_box_right = {};
          source_box_right.left = rec_width; // Offset for the right eye's region
          source_box_right.top = 0;
          source_box_right.front = 0;
          source_box_right.right = rec_width * 2;
          source_box_right.bottom = rec_height;
          source_box_right.back = 1;

          // Copy from main backbuffer to separate eye textures
          D3D::context->CopySubresourceRegion(m_vr_eye_texture_left.Get(), 0, 0, 0, 0, d3d_main_texture, 0, &source_box_left);
          D3D::context->CopySubresourceRegion(m_vr_eye_texture_right.Get(), 0, 0, 0, 0, d3d_main_texture, 0, &source_box_right);

          // Ensure the copy commands are submitted to the GPU before OpenVR tries to use the textures.
          D3D::context->Flush();

          // Submit the distinct eye textures
          if (!vr_system->SubmitFrames(m_vr_eye_texture_left.Get(), m_vr_eye_texture_right.Get()))
          {
            ERROR_LOG_FMT(VR, "Failed to submit distinct eye frames to HMD.");
          }
        }
      }
    }
    else
    {
      ERROR_LOG_FMT(VR, "Main backbuffer texture is null for HMD submission.");
    }
  }

  m_swap_chain->Present();
}

void Gfx::EnsureVREyeTextures(uint32_t width, uint32_t height)
{
  if (m_vr_eye_texture_left && m_vr_eye_texture_right &&
      m_vr_eye_texture_width == width && m_vr_eye_texture_height == height)
  {
    return; // Textures are already valid and correctly sized
  }

  // Release old textures if they exist
  m_vr_eye_texture_left.Reset();
  m_vr_eye_texture_right.Reset();

  DXTexture* main_swapchain_dxtexture = m_swap_chain->GetTexture();
  if (!main_swapchain_dxtexture || !main_swapchain_dxtexture->GetD3DTexture()) {
      ERROR_LOG_FMT(VR, "Cannot create VR eye textures: main swapchain texture is null.");
      return;
  }
  ID3D11Texture2D* d3d_main_texture = static_cast<ID3D11Texture2D*>(main_swapchain_dxtexture->GetD3DTexture());
  D3D11_TEXTURE2D_DESC main_desc;
  d3d_main_texture->GetDesc(&main_desc);

  D3D11_TEXTURE2D_DESC eye_desc = {};
  eye_desc.Width = width;
  eye_desc.Height = height;
  eye_desc.MipLevels = 1;
  eye_desc.ArraySize = 1;
  eye_desc.Format = main_desc.Format; // Use the same format as the backbuffer
  eye_desc.SampleDesc.Count = 1;
  eye_desc.SampleDesc.Quality = 0;
  eye_desc.Usage = D3D11_USAGE_DEFAULT;
  // Bind flags: shader resource is useful for OpenVR, copy destination is implicit with USAGE_DEFAULT for CopySubresourceRegion
  eye_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  eye_desc.CPUAccessFlags = 0;
  eye_desc.MiscFlags = 0;

  HRESULT hr_left = D3D::device->CreateTexture2D(&eye_desc, nullptr, &m_vr_eye_texture_left);
  HRESULT hr_right = D3D::device->CreateTexture2D(&eye_desc, nullptr, &m_vr_eye_texture_right);

  if (SUCCEEDED(hr_left) && SUCCEEDED(hr_right))
  {
    m_vr_eye_texture_width = width;
    m_vr_eye_texture_height = height;
    INFO_LOG_FMT(VR, "Successfully created VR eye textures ({}x{})", width, height);
  }
  else
  {
    m_vr_eye_texture_left.Reset();
    m_vr_eye_texture_right.Reset();
    m_vr_eye_texture_width = 0;
    m_vr_eye_texture_height = 0;
    ERROR_LOG_FMT(VR, "Failed to create VR eye textures. Left HR: {:#0x}, Right HR: {:#0x}", static_cast<u32>(hr_left), static_cast<u32>(hr_right));
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
