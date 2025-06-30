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
}

Gfx::~Gfx()
{
  // Stop VR Presentation Thread if it's running
  if (m_vr_thread_running.load())
  {
    INFO_LOG_FMT(VR, "Gfx::~Gfx - Stopping VR presentation thread...");
    m_vr_thread_running.store(false);
    if (m_vr_presentation_thread.joinable())
    {
      m_vr_presentation_thread.join();
      INFO_LOG_FMT(VR, "Gfx::~Gfx - VR presentation thread joined.");
    }
  }
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
  EnsureVRD3DInitialized(); // Ensure VRD3D is ready if VR mode is active

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
  EnsureVRD3DInitialized(); // Ensure VRD3D is ready if VR mode is active

  if (m_vrd3d)
  {
    // VR Mode: Frame submission is now handled by the VRRenderLoop thread.
    // This function (PresentBackbuffer) is called by the main emulation/render thread.
    // We no longer call m_vrd3d->SubmitFrames() here.
    INFO_LOG_FMT(VR, "Gfx::PresentBackbuffer (VR Path) - Submission handled by VR thread.");

    // TODO: Optionally render a companion view to the main swap chain.
    // For now, just present whatever is on the swap chain.
    // This could be:
    // 1. A blank/cleared screen.
    // 2. A mirror of one of the eye views (would require a CopyResource here from an intermediate texture).
    // 3. A custom companion view.
    // For this refactor, we'll keep it simple and just present the swapchain.
    // If a companion view is desired, the main thread would render it to m_swap_chain->GetFramebuffer()
    // before this PresentBackbuffer call, or we'd copy an eye here.

    if (m_swap_chain)
    {
      // Example: Copy left eye to swap chain for a simple mirror view
      // This is just an illustration; proper implementation would need care.
      /*
      DX11::DXFramebuffer* main_fb = m_swap_chain->GetFramebuffer();
      ID3D11Texture2D* intermediate_left = m_vrd3d->GetD3DLeftEyeIntermediateTexture();
      if (main_fb && main_fb->GetColorAttachment() && intermediate_left)
      {
        ID3D11Texture2D* backbuffer_tex = static_cast<DXTexture*>(main_fb->GetColorAttachment())->GetD3DTexture();
        if (backbuffer_tex)
        {
          // Ensure states are good, then copy. May need to handle different sizes.
          // This is a simplified example.
          D3D::context->CopyResource(backbuffer_tex, intermediate_left);
        }
      }
      */
      m_swap_chain->Present();
    }
  }
  else if (m_swap_chain)
  {
    m_swap_chain->Present();
  }
}

void Gfx::OnConfigChanged(u32 bits)
{
  AbstractGfx::OnConfigChanged(bits);

  // Changed log type to VR for better visibility in dolphin.log for this issue
  INFO_LOG_FMT(VR, "D3DGfx::OnConfigChanged called. Bits: {}", bits);
  if (bits & CONFIG_CHANGE_BIT_STEREO_MODE)
  {
    // Changed log type to VR
    INFO_LOG_FMT(VR, "D3DGfx::OnConfigChanged - StereoMode changed.");
    // EnsureVRD3DInitialized will handle the creation/destruction of m_vrd3d based on new config.
    // We just need to call it to react to the change.
    EnsureVRD3DInitialized(); 

    // Quad-buffer changes require swap chain recreation.
    if (m_swap_chain)
      m_swap_chain->SetStereo(SwapChain::WantsStereo());
  }

  if (bits & CONFIG_CHANGE_BIT_HDR && m_swap_chain)
    m_swap_chain->SetHDR(SwapChain::WantsHDR());

  /*if (bits & CONFIG_CHANGE_BIT_TARGET_SIZE && m_swap_chain)
    m_swap_chain->SetBackbufferScale(TargetSizeUtil::GetBackbufferScale(GetTargetWidth(), GetTargetHeight()));

  if (bits & CONFIG_CHANGE_BIT_VSYNC && m_swap_chain)
    m_swap_chain->SetVSync(g_ActiveConfig.bVSyncActive);

  D3D::stateman->InvalidateConstants();*/
}

void Gfx::EnsureVRD3DInitialized()
{
  // Check if VR mode is active
  if (g_ActiveConfig.stereo_mode == StereoMode::OpenVR)
  {
    // Check if the global VR instance is ready and our D3D VR instance isn't created yet
    if (Core::g_vr_openvr_instance && Core::g_vr_openvr_instance->IsInitialized())
    {
      if (!m_vrd3d) // VRD3D not created yet
      {
        INFO_LOG_FMT(VR, "D3DGfx::EnsureVRD3DInitialized - VR active and m_vrd3d not set. Creating VRD3D.");
        m_vrd3d = std::make_unique<VRD3D>(Core::g_vr_openvr_instance.get(), D3D::device.Get());
        if (m_vrd3d && m_vrd3d->Init())
        {
          INFO_LOG_FMT(VR, "D3DGfx::EnsureVRD3DInitialized - VRD3D initialized successfully.");
          // Start the VR presentation thread if it's not already running
          if (!m_vr_thread_running.load())
          {
            INFO_LOG_FMT(VR, "D3DGfx::EnsureVRD3DInitialized - Starting VR presentation thread.");
            m_vr_thread_running.store(true);
            m_vr_presentation_thread = std::thread(&Gfx::VRRenderLoop, this);
          }
        }
        else
        {
          ERROR_LOG_FMT(VR, "D3DGfx::EnsureVRD3DInitialized - Failed to initialize VRD3D. VR will be non-functional.");
          m_vrd3d.reset(); // Ensure it's null if init failed
          // If VRD3D failed, ensure thread is stopped if it somehow was started or running from a previous state
          if (m_vr_thread_running.load())
          {
            INFO_LOG_FMT(VR, "D3DGfx::EnsureVRD3DInitialized - Stopping VR presentation thread due to VRD3D init failure.");
            m_vr_thread_running.store(false);
            if (m_vr_presentation_thread.joinable())
            {
              m_vr_presentation_thread.join();
            }
          }
        }
      }
      // else m_vrd3d already exists, and thread should be running. No action needed.
    }
    else // Global VR instance not ready
    {
      if (m_vrd3d) // If our VRD3D instance was somehow active, shut it down
      {
        INFO_LOG_FMT(VR, "D3DGfx::EnsureVRD3DInitialized - Global VR instance not ready, but m_vrd3d exists. Resetting VRD3D and stopping thread.");
        if (m_vr_thread_running.load())
        {
          m_vr_thread_running.store(false);
          if (m_vr_presentation_thread.joinable())
          {
            m_vr_presentation_thread.join();
          }
        }
        m_vrd3d.reset();
      }
    }
  }
  else // VR mode is not OpenVR
  {
    if (m_vrd3d) // If it was previously active, shut it down
    {
      INFO_LOG_FMT(VR, "D3DGfx::EnsureVRD3DInitialized - StereoMode is not OpenVR. Resetting m_vrd3d and stopping VR thread.");
      if (m_vr_thread_running.load())
      {
        m_vr_thread_running.store(false);
        if (m_vr_presentation_thread.joinable())
        {
          m_vr_presentation_thread.join();
        }
      }
      m_vrd3d.reset();
    }
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

bool Gfx::SetLeftEyeRenderTarget(const ClearColor& clear_color)
{
  EnsureVRD3DInitialized(); // Ensure VRD3D is ready if VR mode is active
  if (!m_vrd3d)
  {
    // Changed log type to VR
    ERROR_LOG_FMT(VR, "SetLeftEyeRenderTarget called but VRD3D is not initialized (even after EnsureVRD3DInitialized).");
    return false;
  }

  DX11::DXTexture* left_eye_texture = m_vrd3d->GetLeftEyeTexture();
  if (!left_eye_texture)
  {
    // Changed log type to VR
    ERROR_LOG_FMT(VR, "SetLeftEyeRenderTarget: Failed to get left eye texture from VRD3D.");
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
    // Changed log type to VR
    ERROR_LOG_FMT(VR, "SetLeftEyeRenderTarget: Failed to get left eye framebuffer from VRD3D.");
    return false;
  }
  SetAndClearFramebuffer(left_fb, clear_color);
  return true;
}

bool Gfx::SetRightEyeRenderTarget(const ClearColor& clear_color)
{
  EnsureVRD3DInitialized(); // Ensure VRD3D is ready if VR mode is active
  if (!m_vrd3d)
  {
    // Changed log type to VR
    ERROR_LOG_FMT(VR, "SetRightEyeRenderTarget called but VRD3D is not initialized (even after EnsureVRD3DInitialized).");
    return false;
  }

  DX11::DXFramebuffer* right_fb = m_vrd3d->GetRightEyeFramebuffer();
  if (!right_fb)
  {
    // Changed log type to VR
    ERROR_LOG_FMT(VR, "SetRightEyeRenderTarget: Failed to get right eye framebuffer from VRD3D.");
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

bool Gfx::IsVRMode() const
{
  return m_vrd3d != nullptr;
}

void Gfx::VRRenderLoop()
{
  INFO_LOG_FMT(VR, "VRRenderLoop started.");

  // Ensure OpenVR is available
  if (!Core::g_vr_openvr_instance || !Core::g_vr_openvr_instance->IsInitialized() || !Core::g_vr_openvr_instance->GetCompositor())
  {
    ERROR_LOG_FMT(VR, "VRRenderLoop: OpenVR system or compositor not available. Exiting thread.");
    m_vr_thread_running.store(false); // Ensure flag is cleared if we exit early
    return;
  }

  vr::IVRCompositor* compositor = Core::g_vr_openvr_instance->GetCompositor();
  vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]; // For WaitGetPoses

  while (m_vr_thread_running.load())
  {
    INFO_LOG_FMT(VR, "VRRenderLoop: Top of loop, about to call WaitGetPoses.");
    vr::EVRCompositorError pose_error = compositor->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    if (pose_error != vr::VRCompositorError_None)
    {
      ERROR_LOG_FMT(VR, "VRRenderLoop: WaitGetPoses failed with error: {}. Stopping VR thread.", static_cast<int>(pose_error));
      m_vr_thread_running.store(false); // Signal thread to stop
      break; 
    }
    INFO_LOG_FMT(VR, "VRRenderLoop: WaitGetPoses successful.");

    // VROpenVR::GetHMDPose() is called by the main emulation thread to get updated poses for rendering logic.
    // The poses obtained here are mainly for timing via WaitGetPoses.

    { // This block defines the scope of the unique_lock
      std::unique_lock<std::mutex> lock(m_d3d_context_mutex);
      INFO_LOG_FMT(VR, "VRRenderLoop: Acquired lock, about to wait on CV.");
      
      m_vr_frame_ready_cv.wait(lock, [&] {
        INFO_LOG_FMT(VR, "VRRenderLoop: CV Predicate Check: m_vr_new_frame_rendered={}, m_vr_thread_running={}", m_vr_new_frame_rendered.load(), m_vr_thread_running.load());
        return m_vr_new_frame_rendered.load() || !m_vr_thread_running.load();
      });
      INFO_LOG_FMT(VR, "VRRenderLoop: Woke from CV wait. m_vr_new_frame_rendered={}, m_vr_thread_running={}", m_vr_new_frame_rendered.load(), m_vr_thread_running.load());

      if (!m_vr_thread_running.load())
      {
        INFO_LOG_FMT(VR, "VRRenderLoop: Shutdown signaled while waiting for frame. Exiting loop.");
        break; // lock is released as it goes out of scope
      }

      // At this point, m_vr_new_frame_rendered is true, and we hold the lock.
      if (m_vrd3d)
      {
        INFO_LOG_FMT(VR, "VRRenderLoop: Preparing to copy resources.");
        // 1. Copy Resources
        ID3D11Texture2D* left_intermediate = m_vrd3d->GetD3DLeftEyeIntermediateTexture();
        ID3D11Texture2D* right_intermediate = m_vrd3d->GetD3DRightEyeIntermediateTexture();
        ID3D11Texture2D* left_submit = m_vrd3d->GetD3DLeftEyeSubmitTexture();
        ID3D11Texture2D* right_submit = m_vrd3d->GetD3DRightEyeSubmitTexture();

        INFO_LOG_FMT(VR, "VRRenderLoop: Texture Pointers - L_Intermediate: {}, L_Submit: {}, R_Intermediate: {}, R_Submit: {}",
                     fmt::ptr(left_intermediate), fmt::ptr(left_submit), fmt::ptr(right_intermediate), fmt::ptr(right_submit));

        if (left_intermediate && left_submit)
        {
          D3D::context->CopyResource(left_submit, left_intermediate);
          INFO_LOG_FMT(VR, "VRRenderLoop: CopyResource called for left eye.");
        }
        else
        {
          ERROR_LOG_FMT(VR, "VRRenderLoop: Missing left eye textures for copy. Intermediate: {}, Submit: {}", fmt::ptr(left_intermediate), fmt::ptr(left_submit));
        }

        if (right_intermediate && right_submit)
        {
          D3D::context->CopyResource(right_submit, right_intermediate);
          INFO_LOG_FMT(VR, "VRRenderLoop: CopyResource called for right eye.");
        }
        else
        {
          ERROR_LOG_FMT(VR, "VRRenderLoop: Missing right eye textures for copy. Intermediate: {}, Submit: {}", fmt::ptr(right_intermediate), fmt::ptr(right_submit));
        }

        // 2. Submit Frames
        INFO_LOG_FMT(VR, "VRRenderLoop: Preparing to submit frames.");
        if (!m_vrd3d->SubmitFrames())
        {
          ERROR_LOG_FMT(VR, "VRRenderLoop: m_vrd3d->SubmitFrames() failed.");
        }
        else
        {
          INFO_LOG_FMT(VR, "VRRenderLoop: m_vrd3d->SubmitFrames() successful.");
        }
      }
      else
      {
        ERROR_LOG_FMT(VR, "VRRenderLoop: m_vrd3d is null during D3D operations. This should not happen if thread lifecycle is correct.");
        m_vr_thread_running.store(false); // Critical error, signal shutdown
        break; 
      }
      
      // Reset the flag indicating that this frame has been consumed.
      m_vr_new_frame_rendered.store(false);
      INFO_LOG_FMT(VR, "VRRenderLoop: Reset m_vr_new_frame_rendered to false.");
      
      // The unique_lock 'lock' is released automatically when this scope ends.
    } 
    INFO_LOG_FMT(VR, "VRRenderLoop: End of loop iteration, lock released.");
  } // End of while(m_vr_thread_running.load())

  INFO_LOG_FMT(VR, "VRRenderLoop ended.");
}

}  // namespace DX11
