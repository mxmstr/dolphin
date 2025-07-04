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

ID3D11PixelShader* Gfx::s_osvr_distortion_shader = nullptr;

const char osvr_program_code[] = {
    "sampler samp0 : register(s0);\n"
    "Texture2DArray Tex0 : register(t0);\n"

    "float2 Distort(float2 p, float k1){\n"
    "	float r2 = p.x * p.x + p.y * p.y;\n"
    "	float r = sqrt(r2);\n"
    "	float newRadius = (1 + k1*r*r);\n"
    "	p.x = p.x * newRadius;\n"
    "	p.y = p.y * newRadius;\n"
    "	return p;\n"
    "}\n"

    "void main(\n"
    "out float4 ocol0 : SV_Target,\n"
    "in float4 pos : SV_Position,\n"
    "in float3 uv0 : TEXCOORD0){\n"
    "	float2 uv_red, uv_green, uv_blue;\n"
    "	float4 color_red, color_green, color_blue;\n"
    "	float2 sectorOrigin;\n"

    "	if (uv0.z > 0.0)\n" // Assuming uv0.z indicates eye index (0 for left, >0 for right)
    "		sectorOrigin = float2(1.0 - 0.47, 0.55); // Values from Hydra, might need adjustment\n"
    "	else\n"
    "		sectorOrigin = float2(0.47, 0.55);\n"

    "	uv_red		= Distort(uv0.xy - sectorOrigin, 0.45) + sectorOrigin;\n"
    "	uv_green	= Distort(uv0.xy - sectorOrigin, 0.53) + sectorOrigin;\n"
    "	uv_blue		= Distort(uv0.xy - sectorOrigin, 0.66) + sectorOrigin;\n"

    "	color_red = Tex0.Sample(samp0, float3(uv_red, uv0.z));\n"
    "	color_green = Tex0.Sample(samp0, float3(uv_green, uv0.z));\n"
    "	color_blue = Tex0.Sample(samp0, float3(uv_blue, uv0.z));\n"

    "	if (((uv_red.x>0) && (uv_red.x<1) && (uv_red.y>0) && (uv_red.y<1)))\n"
    "		ocol0 = float4(color_red.x, color_green.y, color_blue.z, 1.0);\n"
    "	else\n"
    "		ocol0 = float4(0, 0, 0, 0); //black\n"
    "}\n"};

void Gfx::InitUtilityShaders()
{
  auto bytecode = DXShader::CompileShader(D3D::feature_level, ShaderStage::Pixel, osvr_program_code);
  if (bytecode)
  {
    s_osvr_distortion_shader = static_cast<ID3D11PixelShader*>(
        DXShader::CreateFromBytecode(ShaderStage::Pixel, std::move(*bytecode), "OSVRDistortion")->GetD3DPixelShader());
    // The shader object is now owned by s_osvr_distortion_shader, no need to release the unique_ptr explicitly
    // as it goes out of scope. The underlying D3D resource is AddRef'd.
  }
  if (!s_osvr_distortion_shader)
  {
    PanicAlertFmt("Failed to compile OSVR distortion pixel shader.");
  }
}

void Gfx::ShutdownUtilityShaders()
{
  SAFE_RELEASE(s_osvr_distortion_shader);
}

ID3D11PixelShader* Gfx::GetOSVRDistortionShader()
{
  return s_osvr_distortion_shader;
}

Gfx::Gfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale)
    : m_backbuffer_scale(backbuffer_scale), m_swap_chain(std::move(swap_chain))
{
  InitUtilityShaders(); // Initialize utility shaders including OSVR
  if (g_ActiveConfig.bEnableVR)
  {
    // Initialize VR system
    VR_Init(); // From VR.cpp - initializes SDKs
    if (g_has_hmd) // g_has_hmd is set within VR_Init if an HMD is detected
    {
      m_stereo3d = true;
      m_eye_count = 2; // Assuming 2 eyes for typical VR HMDs
      VR_ConfigureHMD(); // From VRD3D.cpp - specific D3D configuration for HMD
      VR_StartFramebuffer(); // From VRD3D.cpp - creates eye textures
    }
    else
    {
      m_stereo3d = false;
      m_eye_count = 1;
    }
  }
}

Gfx::~Gfx()
{
  ShutdownUtilityShaders(); // Shutdown utility shaders
  if (g_ActiveConfig.bEnableVR && g_has_hmd)
  {
    VR_StopFramebuffer(); // From VRD3D.cpp - releases eye textures
    VR_StopRendering();   // From VR.cpp (potentially, or VRD3D) - platform-agnostic VR rendering shutdown
    VR_Shutdown();        // From VR.cpp - shuts down VR SDKs
  }
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
  SetAndClearFramebuffer(m_swap_chain->GetFramebuffer(), clear_color);
  return true;
}

void Gfx::PresentBackbuffer()
{
  m_swap_chain->Present();
}

void Gfx::PresentBackbuffer()
{
  if (g_ActiveConfig.bEnableVR && g_has_hmd && m_stereo3d)
  {
    // VR Rendering Path
    VR_BeginFrame(); // Generic SDK call
    VR_GetEyePoses(); // Generic SDK call to get latest HMD poses

    // Get EFB texture (resolved if MSAA is active)
    AbstractTexture* efb_abstract_texture = g_framebuffer_manager->IsEFBMultisampled() ?
                                            g_framebuffer_manager->ResolveEFBColorTexture({}) : // Resolve whole EFB
                                            g_framebuffer_manager->GetEFBColorTexture();

    DXTexture* efb_dx_texture = static_cast<DXTexture*>(efb_abstract_texture);
    if (!efb_dx_texture || !efb_dx_texture->GetD3DSRV())
    {
      ERROR_LOG(VR_D3D, "Failed to get EFB texture for VR rendering.");
      // Fallback to normal presentation or error out? For now, try to present to screen.
      if (m_swap_chain)
        m_swap_chain->Present();
      return;
    }

    // Define EFB source rectangle (full EFB)
    D3D11_RECT efb_source_rect = CD3D11_RECT(0, 0, efb_dx_texture->GetWidth(), efb_dx_texture->GetHeight());
    UINT efb_source_width = efb_dx_texture->GetWidth();
    UINT efb_source_height = efb_dx_texture->GetHeight();

    // TODO: AvatarDrawer would be drawn here if ported and active, likely before eye loops or per-eye.
    // s_avatarDrawer.Draw(); // From Hydra

    for (int eye = 0; eye < m_eye_count; ++eye)
    {
      VR_D3D_RenderToEyeBuffer(this, eye);

      // TODO: Set up viewport for this eye texture if it's different from EFB size
      // For now, assume eye buffer is same size as EFB for direct blit.
      D3D11_VIEWPORT eye_viewport = CD3D11_VIEWPORT(0.0f, 0.0f,
                                                 (float)m_frontBuffer[eye]->GetWidth(),
                                                 (float)m_frontBuffer[eye]->GetHeight(),
                                                 0.0f, 1.0f);
      D3D::context->RSSetViewports(1, &eye_viewport);

      // TODO: Correctly set up shaders and constants for VR rendering for this eye.
      // This is a simplified blit for now. Hydra's VertexShaderManager::SetConstants
      // handled complex VR matrix transformations.
      // For a simple blit, we might use a standard textured quad shader.
      // The `g_renderer->GetScreenQuadPixelShader()` and `g_renderer->GetScreenQuadVertexShader()`
      // might be suitable if they are simple pass-throughs.
      // Gamma is 1.0f for direct copy. The 'eye' parameter to drawShadedTexQuad in Hydra
      // was used to select the correct part of a stereo texture or apply eye-specific transforms.
      // Here, we are rendering to separate eye textures.

      // Placeholder: Using a generic quad draw. This needs proper shader setup for VR.
      // This assumes a function similar to Hydra's D3D::drawShadedTexQuad.
      // We need to ensure VideoCommon or D3DCommon provides such a utility or adapt it.
      // For now, this is a conceptual blit.
      D3DCommon::DrawVideoQuad(efb_dx_texture->GetD3DSRV(),
                               efb_source_rect,
                               m_frontBuffer[eye]->GetWidth(), m_frontBuffer[eye]->GetHeight(),
                               0.0f, // sx_scale
                               0.0f, // sy_scale
                               1.0f, // Gamma,
                               0,    // slice for array textures, or eye index for stereo shaders
                               D3DCommon::MONO_VIDEO_QUAD); // Shader type - needs a stereo/VR version or parameter

      // TODO: Draw OSD messages per eye if needed
      // OSD::DrawMessages(); // This would need to be adapted for VR context
    }

    VR_D3D_SubmitFrameToHMD(); // Submits m_frontBuffer[0] and m_frontBuffer[1]

    // Synchronous Timewarp
    if (g_ActiveConfig.bSynchronousTimewarp)
    {
        // Simplified timewarp logic from Hydra. Actual frame rate calculation would be needed.
        // This is just a placeholder for the loop.
        int extra_timewarp_frames = 0;
        if (g_ActiveConfig.iExtraTimewarpedFrames > 0) { // Use the config value directly if set
            extra_timewarp_frames = g_ActiveConfig.iExtraTimewarpedFrames;
        } else {
            // Basic dynamic calculation (needs proper FPS tracking)
            // float current_fps = Core::GetFPS(); // Hypothetical FPS getter
            // if (current_fps > 0 && g_ActiveConfig.HMD_refresh_rate > current_fps) {
            //    extra_timewarp_frames = static_cast<int>(g_ActiveConfig.HMD_refresh_rate / current_fps) -1;
            // }
        }

        for (int i = 0; i < extra_timewarp_frames; ++i)
        {
            if (!VR_GetShouldQuit()) // VR_GetShouldQuit from VideoCommon/VR.h
            {
                VR_GetEyePoses(); // Update poses for timewarp
                VR_D3D_DrawTimewarpFrame(this);
            } else {
                break;
            }
        }
    }
    // End of VR Rendering Path
  }
  else
  {
    // Original PresentBackbuffer logic for non-VR
    if (m_swap_chain)
      m_swap_chain->Present();
  }
}


void Gfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

  // Quad-buffer changes require swap chain recreation.
  if (bits & CONFIG_CHANGE_BIT_STEREO_MODE && m_swap_chain)
  {
    m_swap_chain->SetStereo(SwapChain::WantsStereo());
    // If VR is being enabled/disabled, we might need to re-init parts of VR system
    if(g_ActiveConfig.bEnableVR && !m_stereo3d) { // VR just got enabled
        VR_Init();
        if(g_has_hmd){
            m_stereo3d = true;
            m_eye_count = 2;
            VR_D3D_ConfigureHMD();
            VR_D3D_StartFramebuffer(this);
        }
    } else if (!g_ActiveConfig.bEnableVR && m_stereo3d) { // VR just got disabled
        if(g_has_hmd){
            VR_D3D_StopFramebuffer(this);
            VR_StopRendering(); // Generic
            VR_Shutdown(); // Generic
        }
        m_stereo3d = false;
        m_eye_count = 1;
    }
  }

  if (bits & CONFIG_CHANGE_BIT_HDR && m_swap_chain)
    m_swap_chain->SetHDR(SwapChain::WantsHDR());

  if (bits & CONFIG_CHANGE_BIT_TARGET_SIZE && g_ActiveConfig.bEnableVR && m_stereo3d && g_has_hmd)
  {
    // Recreate eye buffers if target size changed
    VR_D3D_StopFramebuffer(this);
    VR_D3D_StartFramebuffer(this);
  }
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
