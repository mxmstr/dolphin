// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexShaderManager.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iterator>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Matrix.h"
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FreeLookCamera.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexManagerBase.h"
#include <VideoCommon/VideoBackendBase.h>
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"
#include <openvr.h>

void VertexShaderManager::Init()
{
  // Initialize state tracking variables
  m_projection_graphics_mod_change = false;

  constants = {};

  m_projection_matrix = Common::Matrix44::Identity().data;

  dirty = true;
}

Common::Matrix44 VertexShaderManager::LoadProjectionMatrix()
{
  const auto& rawProjection = xfmem.projection.rawProjection;

  switch (xfmem.projection.type)
  {
  case ProjectionType::Perspective:
  {
    const Common::Vec2 fov_multiplier = g_freelook_camera.IsActive() ?
                                            g_freelook_camera.GetFieldOfViewMultiplier() :
                                            Common::Vec2{1, 1};
    m_projection_matrix[0] = rawProjection[0] * g_ActiveConfig.fAspectRatioHackW * fov_multiplier.x;
    m_projection_matrix[1] = 0.0f;
    m_projection_matrix[2] = rawProjection[1] * g_ActiveConfig.fAspectRatioHackW * fov_multiplier.x;
    m_projection_matrix[3] = 0.0f;

    m_projection_matrix[4] = 0.0f;
    m_projection_matrix[5] = rawProjection[2] * g_ActiveConfig.fAspectRatioHackH * fov_multiplier.y;
    m_projection_matrix[6] = rawProjection[3] * g_ActiveConfig.fAspectRatioHackH * fov_multiplier.y;
    m_projection_matrix[7] = 0.0f;

    m_projection_matrix[8] = 0.0f;
    m_projection_matrix[9] = 0.0f;
    m_projection_matrix[10] = rawProjection[4];
    m_projection_matrix[11] = rawProjection[5];

    m_projection_matrix[12] = 0.0f;
    m_projection_matrix[13] = 0.0f;

    m_projection_matrix[14] = -1.0f;
    m_projection_matrix[15] = 0.0f;

    g_stats.gproj = m_projection_matrix;
  }
  break;

  case ProjectionType::Orthographic:
  {
    m_projection_matrix[0] = rawProjection[0];
    m_projection_matrix[1] = 0.0f;
    m_projection_matrix[2] = 0.0f;
    m_projection_matrix[3] = rawProjection[1];

    m_projection_matrix[4] = 0.0f;
    m_projection_matrix[5] = rawProjection[2];
    m_projection_matrix[6] = 0.0f;
    m_projection_matrix[7] = rawProjection[3];

    m_projection_matrix[8] = 0.0f;
    m_projection_matrix[9] = 0.0f;
    m_projection_matrix[10] = rawProjection[4];
    m_projection_matrix[11] = rawProjection[5];

    m_projection_matrix[12] = 0.0f;
    m_projection_matrix[13] = 0.0f;

    m_projection_matrix[14] = 0.0f;
    m_projection_matrix[15] = 1.0f;

    g_stats.g2proj = m_projection_matrix;
    g_stats.proj = rawProjection;
  }
  break;

  default:
    ERROR_LOG_FMT(VIDEO, "Unknown projection type: {}", xfmem.projection.type);
  }

  PRIM_LOG("Projection: {} {} {} {} {} {}", rawProjection[0], rawProjection[1], rawProjection[2],
           rawProjection[3], rawProjection[4], rawProjection[5]);

  auto corrected_matrix = Common::Matrix44::FromArray(m_projection_matrix);

  if (g_freelook_camera.IsActive() && xfmem.projection.type == ProjectionType::Perspective)
    corrected_matrix *= g_freelook_camera.GetView();

  g_freelook_camera.GetController()->SetClean();

  return corrected_matrix;
}

static Common::Matrix44 ConvertHmdMatrix34ToMatrix44(const vr::HmdMatrix34_t& mat34)
{
  return Common::Matrix44::FromArray({{
    mat34.m[0][0], mat34.m[0][1], mat34.m[0][2], mat34.m[0][3],
    mat34.m[1][0], mat34.m[1][1], mat34.m[1][2], mat34.m[1][3],
    mat34.m[2][0], mat34.m[2][1], mat34.m[2][2], mat34.m[2][3],
    0.0f,          0.0f,          0.0f,          1.0f
  }});
}

void VertexShaderManager::SetProjectionMatrix(XFStateManager& xf_state_manager)
{
  // Condition to update:
  // 1. XFMem projection changed
  // 2. FreeLook camera is dirty (moved/rotated)
  // 3. OR VR mode is active (because HMD pose can change every frame)
  bool needs_update = xf_state_manager.DidProjectionChange() ||
                      g_freelook_camera.GetController()->IsDirty() ||
                      (g_ActiveConfig.stereo_mode == StereoMode::OpenVR);

  if (needs_update)
  {
    xf_state_manager.ResetProjection(); // Clear the XFStateManager flag if it was set

    // Load the game's base projection matrix (incorporates game settings, aspect hack, freelook if active)
    // This matrix is effectively Projection_Game * View_FreeLook (if freelook active)
    Common::Matrix44 game_final_mono_projection = LoadProjectionMatrix();

    if (g_ActiveConfig.stereo_mode == StereoMode::OpenVR && g_video_backend->GetVROpenVR() && g_video_backend->GetVROpenVR()->IsInitialized())
    {
      VROpenVR* vr_system = g_video_backend->GetVROpenVR(); // Assume g_pVROpenVR is how we access it
      Common::Matrix44 hmd_pose_matrix;

      // TODO: Determine actual prediction time. Using 0.0f for now.
      if (vr_system->GetHMDPose(0.0f, hmd_pose_matrix))
      {
        Common::Matrix44 inv_hmd_pose_matrix = hmd_pose_matrix.Inverted();

        // Get near/far from xfmem for OpenVR projection matrices
        // xfmem.projection.nearz and farz are positive values.
        float near_clip = 0.0f;
        float far_clip = 0.0f;

        if (xfmem.projection.type == ProjectionType::Perspective)
        {
          // Assuming rawProjection[4] and rawProjection[5] encode near and far planes
          near_clip = xfmem.projection.rawProjection[4];
          far_clip = xfmem.projection.rawProjection[5];
        }
        else
        {
          // For orthographic projections, near and far planes might be encoded differently
          near_clip = xfmem.projection.rawProjection[4];
          far_clip = xfmem.projection.rawProjection[5];
        }

        if (near_clip <= 0.0f) near_clip = 0.01f; // OpenVR requires positive near clip
        if (far_clip <= near_clip) far_clip = near_clip + 100.0f;


        Common::Matrix44 left_eye_openvr_proj, right_eye_openvr_proj;
        vr_system->GetEyeProjectionMatrix(vr::Eye_Left, near_clip, far_clip, left_eye_openvr_proj);
        vr_system->GetEyeProjectionMatrix(vr::Eye_Right, near_clip, far_clip, right_eye_openvr_proj);

        vr::HmdMatrix34_t raw_left_eye_to_head = vr_system->GetIVRSystem()->GetEyeToHeadTransform(vr::Eye_Left);
        Common::Matrix44 left_eye_to_head_matrix = ConvertHmdMatrix34ToMatrix44(raw_left_eye_to_head);
        Common::Matrix44 inv_left_eye_to_head_matrix = left_eye_to_head_matrix.Inverted();

        vr::HmdMatrix34_t raw_right_eye_to_head = vr_system->GetIVRSystem()->GetEyeToHeadTransform(vr::Eye_Right);
        Common::Matrix44 right_eye_to_head_matrix = ConvertHmdMatrix34ToMatrix44(raw_right_eye_to_head);
        Common::Matrix44 inv_right_eye_to_head_matrix = right_eye_to_head_matrix.Inverted();

        // Final projection for each eye: P_openvr_eye * Inv(T_eye_to_head) * Inv(T_hmd_world)
        // This resulting matrix will transform from world space to clip space for that eye.
        // The game's original view transform is implicitly part of xfmem.posMatrices / I_POSNORMALMATRIX,
        // which takes model space to world space.
        Common::Matrix44 final_left_projection = left_eye_openvr_proj * inv_left_eye_to_head_matrix * inv_hmd_pose_matrix;
        Common::Matrix44 final_right_projection = right_eye_openvr_proj * inv_right_eye_to_head_matrix * inv_hmd_pose_matrix;

        memcpy(constants.projection_left.data(), final_left_projection.data.data(), 4 * sizeof(float4));
        memcpy(constants.projection_right.data(), final_right_projection.data.data(), 4 * sizeof(float4));

        // Set the main 'projection' to the left eye for any non-shader code that might peek at it,
        // or for UIs rendered in mono over the VR scene.
        memcpy(constants.projection.data(), final_left_projection.data.data(), 4 * sizeof(float4));
      }
      else
      {
        // VR mode active but HMD pose failed, use mono game projection for both eyes
        memcpy(constants.projection.data(), game_final_mono_projection.data.data(), 4 * sizeof(float4));
        memcpy(constants.projection_left.data(), game_final_mono_projection.data.data(), 4 * sizeof(float4));
        memcpy(constants.projection_right.data(), game_final_mono_projection.data.data(), 4 * sizeof(float4));
      }
    }
    else
    {
      // Not in OpenVR mode, or VROpenVR not initialized. Use game's mono projection.
      memcpy(constants.projection.data(), game_final_mono_projection.data.data(), 4 * sizeof(float4));
      // For other stereo modes (like anaglyph, SBS) that might be adapted to use these new UBO fields,
      // initialize them to the mono projection. The Geometry Shader's I_STEREOPARAMS logic would still work.
      memcpy(constants.projection_left.data(), game_final_mono_projection.data.data(), 4 * sizeof(float4));
      memcpy(constants.projection_right.data(), game_final_mono_projection.data.data(), 4 * sizeof(float4));
    }
    dirty = true; // Mark UBO as dirty because some projection has been updated
  }
}

bool VertexShaderManager::UseVertexDepthRange()
{
  // We can't compute the depth range in the vertex shader if we don't support depth clamp.
  if (!g_backend_info.bSupportsDepthClamp)
    return false;

  // We need a full depth range if a ztexture is used.
  if (bpmem.ztex2.op != ZTexOp::Disabled && !bpmem.zcontrol.early_ztest)
    return true;

  // If an inverted depth range is unsupported, we also need to check if the range is inverted.
  if (!g_backend_info.bSupportsReversedDepthRange)
  {
    if (xfmem.viewport.zRange < 0.0f)
      return true;

    if (xfmem.viewport.zRange > xfmem.viewport.farZ)
      return true;
  }

  // If an oversized depth range or a ztexture is used, we need to calculate the depth range
  // in the vertex shader.
  return fabs(xfmem.viewport.zRange) > 16777215.0f || fabs(xfmem.viewport.farZ) > 16777215.0f;
}

// Syncs the shader constant buffers with xfmem
// TODO: A cleaner way to control the matrices without making a mess in the parameters field
void VertexShaderManager::SetConstants(const std::vector<std::string>& textures,
                                       XFStateManager& xf_state_manager)
{
  if (constants.missing_color_hex != g_ActiveConfig.iMissingColorValue)
  {
    const float a = (g_ActiveConfig.iMissingColorValue) & 0xFF;
    const float b = (g_ActiveConfig.iMissingColorValue >> 8) & 0xFF;
    const float g = (g_ActiveConfig.iMissingColorValue >> 16) & 0xFF;
    const float r = (g_ActiveConfig.iMissingColorValue >> 24) & 0xFF;
    constants.missing_color_hex = g_ActiveConfig.iMissingColorValue;
    constants.missing_color_value = {r / 255, g / 255, b / 255, a / 255};

    dirty = true;
  }

  const auto per_vertex_transform_matrix_changes =
      xf_state_manager.GetPerVertexTransformMatrixChanges();
  if (per_vertex_transform_matrix_changes[0] >= 0)
  {
    int startn = per_vertex_transform_matrix_changes[0] / 4;
    int endn = (per_vertex_transform_matrix_changes[1] + 3) / 4;
    memcpy(constants.transformmatrices[startn].data(), &xfmem.posMatrices[startn * 4],
           (endn - startn) * sizeof(float4));
    dirty = true;
    xf_state_manager.ResetPerVertexTransformMatrixChanges();
  }

  const auto per_vertex_normal_matrices_changed =
      xf_state_manager.GetPerVertexNormalMatrixChanges();
  if (per_vertex_normal_matrices_changed[0] >= 0)
  {
    int startn = per_vertex_normal_matrices_changed[0] / 3;
    int endn = (per_vertex_normal_matrices_changed[1] + 2) / 3;
    for (int i = startn; i < endn; i++)
    {
      memcpy(constants.normalmatrices[i].data(), &xfmem.normalMatrices[3 * i], 12);
    }
    dirty = true;
    xf_state_manager.ResetPerVertexNormalMatrixChanges();
  }

  const auto post_transform_matrices_changed = xf_state_manager.GetPostTransformMatrixChanges();
  if (post_transform_matrices_changed[0] >= 0)
  {
    int startn = post_transform_matrices_changed[0] / 4;
    int endn = (post_transform_matrices_changed[1] + 3) / 4;
    memcpy(constants.posttransformmatrices[startn].data(), &xfmem.postMatrices[startn * 4],
           (endn - startn) * sizeof(float4));
    dirty = true;
    xf_state_manager.ResetPostTransformMatrixChanges();
  }

  const auto light_changes = xf_state_manager.GetLightsChanged();
  if (light_changes[0] >= 0)
  {
    // TODO: Outdated comment
    // lights don't have a 1 to 1 mapping, the color component needs to be converted to 4 floats
    const int istart = light_changes[0] / 0x10;
    const int iend = (light_changes[1] + 15) / 0x10;

    for (int i = istart; i < iend; ++i)
    {
      const Light& light = xfmem.lights[i];
      VertexShaderConstants::Light& dstlight = constants.lights[i];

      // xfmem.light.color is packed as abgr in u8[4], so we have to swap the order
      dstlight.color[0] = light.color[3];
      dstlight.color[1] = light.color[2];
      dstlight.color[2] = light.color[1];
      dstlight.color[3] = light.color[0];

      dstlight.cosatt[0] = light.cosatt[0];
      dstlight.cosatt[1] = light.cosatt[1];
      dstlight.cosatt[2] = light.cosatt[2];

      if (fabs(light.distatt[0]) < 0.00001f && fabs(light.distatt[1]) < 0.00001f &&
          fabs(light.distatt[2]) < 0.00001f)
      {
        // dist attenuation, make sure not equal to 0!!!
        dstlight.distatt[0] = .00001f;
      }
      else
      {
        dstlight.distatt[0] = light.distatt[0];
      }
      dstlight.distatt[1] = light.distatt[1];
      dstlight.distatt[2] = light.distatt[2];

      dstlight.pos[0] = light.dpos[0];
      dstlight.pos[1] = light.dpos[1];
      dstlight.pos[2] = light.dpos[2];

      // TODO: Hardware testing is needed to confirm that this normalization is correct
      auto sanitize = [](float f) {
        if (std::isnan(f))
          return 0.0f;
        else if (std::isinf(f))
          return f > 0.0f ? 1.0f : -1.0f;
        else
          return f;
      };
      double norm = double(light.ddir[0]) * double(light.ddir[0]) +
                    double(light.ddir[1]) * double(light.ddir[1]) +
                    double(light.ddir[2]) * double(light.ddir[2]);
      norm = 1.0 / sqrt(norm);
      dstlight.dir[0] = sanitize(static_cast<float>(light.ddir[0] * norm));
      dstlight.dir[1] = sanitize(static_cast<float>(light.ddir[1] * norm));
      dstlight.dir[2] = sanitize(static_cast<float>(light.ddir[2] * norm));
    }
    dirty = true;

    xf_state_manager.ResetLightsChanged();
  }

  for (int i : xf_state_manager.GetMaterialChanges())
  {
    u32 data = i >= 2 ? xfmem.matColor[i - 2] : xfmem.ambColor[i];
    constants.materials[i][0] = (data >> 24) & 0xFF;
    constants.materials[i][1] = (data >> 16) & 0xFF;
    constants.materials[i][2] = (data >> 8) & 0xFF;
    constants.materials[i][3] = data & 0xFF;
    dirty = true;
  }
  xf_state_manager.ResetMaterialChanges();

  if (xf_state_manager.DidPosNormalChange())
  {
    xf_state_manager.ResetPosNormalChange();
    const float* pos = &xfmem.posMatrices[g_main_cp_state.matrix_index_a.PosNormalMtxIdx * 4];
    const float* norm =
        &xfmem.normalMatrices[3 * (g_main_cp_state.matrix_index_a.PosNormalMtxIdx & 31)];

    memcpy(constants.posnormalmatrix.data(), pos, 3 * sizeof(float4));
    memcpy(constants.posnormalmatrix[3].data(), norm, 3 * sizeof(float));
    memcpy(constants.posnormalmatrix[4].data(), norm + 3, 3 * sizeof(float));
    memcpy(constants.posnormalmatrix[5].data(), norm + 6, 3 * sizeof(float));
    dirty = true;
  }

  if (xf_state_manager.DidTexMatrixAChange())
  {
    xf_state_manager.ResetTexMatrixAChange();
    const std::array<const float*, 4> pos_matrix_ptrs{
        &xfmem.posMatrices[g_main_cp_state.matrix_index_a.Tex0MtxIdx * 4],
        &xfmem.posMatrices[g_main_cp_state.matrix_index_a.Tex1MtxIdx * 4],
        &xfmem.posMatrices[g_main_cp_state.matrix_index_a.Tex2MtxIdx * 4],
        &xfmem.posMatrices[g_main_cp_state.matrix_index_a.Tex3MtxIdx * 4],
    };

    for (size_t i = 0; i < pos_matrix_ptrs.size(); ++i)
    {
      memcpy(constants.texmatrices[3 * i].data(), pos_matrix_ptrs[i], 3 * sizeof(float4));
    }
    dirty = true;
  }

  if (xf_state_manager.DidTexMatrixBChange())
  {
    xf_state_manager.ResetTexMatrixBChange();
    const std::array<const float*, 4> pos_matrix_ptrs{
        &xfmem.posMatrices[g_main_cp_state.matrix_index_b.Tex4MtxIdx * 4],
        &xfmem.posMatrices[g_main_cp_state.matrix_index_b.Tex5MtxIdx * 4],
        &xfmem.posMatrices[g_main_cp_state.matrix_index_b.Tex6MtxIdx * 4],
        &xfmem.posMatrices[g_main_cp_state.matrix_index_b.Tex7MtxIdx * 4],
    };

    for (size_t i = 0; i < pos_matrix_ptrs.size(); ++i)
    {
      memcpy(constants.texmatrices[3 * i + 12].data(), pos_matrix_ptrs[i], 3 * sizeof(float4));
    }
    dirty = true;
  }

  if (xf_state_manager.DidViewportChange())
  {
    xf_state_manager.ResetViewportChange();

    // The console GPU places the pixel center at 7/12 unless antialiasing
    // is enabled, while D3D and OpenGL place it at 0.5. See the comment
    // in VertexShaderGen.cpp for details.
    // NOTE: If we ever emulate antialiasing, the sample locations set by
    // BP registers 0x01-0x04 need to be considered here.
    const float pixel_center_correction = 7.0f / 12.0f - 0.5f;
    const bool bUseVertexRounding = g_ActiveConfig.UseVertexRounding();
    const float viewport_width = bUseVertexRounding ?
                                     (2.f * xfmem.viewport.wd) :
                                     g_framebuffer_manager->EFBToScaledXf(2.f * xfmem.viewport.wd);
    const float viewport_height = bUseVertexRounding ?
                                      (2.f * xfmem.viewport.ht) :
                                      g_framebuffer_manager->EFBToScaledXf(2.f * xfmem.viewport.ht);
    const float pixel_size_x = 2.f / viewport_width;
    const float pixel_size_y = 2.f / viewport_height;
    constants.pixelcentercorrection[0] = pixel_center_correction * pixel_size_x;
    constants.pixelcentercorrection[1] = pixel_center_correction * pixel_size_y;

    // By default we don't change the depth value at all in the vertex shader.
    constants.pixelcentercorrection[2] = 1.0f;
    constants.pixelcentercorrection[3] = 0.0f;

    constants.viewport[0] = (2.f * xfmem.viewport.wd);
    constants.viewport[1] = (2.f * xfmem.viewport.ht);

    if (UseVertexDepthRange())
    {
      // Oversized depth ranges are handled in the vertex shader. We need to reverse
      // the far value to use the reversed-Z trick.
      if (g_backend_info.bSupportsReversedDepthRange)
      {
        // Sometimes the console also tries to use the reversed-Z trick. We can only do
        // that with the expected accuracy if the backend can reverse the depth range.
        constants.pixelcentercorrection[2] = fabs(xfmem.viewport.zRange) / 16777215.0f;
        if (xfmem.viewport.zRange < 0.0f)
          constants.pixelcentercorrection[3] = xfmem.viewport.farZ / 16777215.0f;
        else
          constants.pixelcentercorrection[3] = 1.0f - xfmem.viewport.farZ / 16777215.0f;
      }
      else
      {
        // For backends that don't support reversing the depth range we can still render
        // cases where the console uses the reversed-Z trick. But we simply can't provide
        // the expected accuracy, which might result in z-fighting.
        constants.pixelcentercorrection[2] = xfmem.viewport.zRange / 16777215.0f;
        constants.pixelcentercorrection[3] = 1.0f - xfmem.viewport.farZ / 16777215.0f;
      }
    }

    dirty = true;
    BPFunctions::SetScissorAndViewport();
    g_stats.AddScissorRect();
  }

  std::vector<GraphicsModAction*> projection_actions;
  if (g_ActiveConfig.bGraphicMods)
  {
    for (const auto& action : g_graphics_mod_manager->GetProjectionActions(xfmem.projection.type))
    {
      projection_actions.push_back(action);
    }

    for (const auto& texture : textures)
    {
      for (const auto& action :
           g_graphics_mod_manager->GetProjectionTextureActions(xfmem.projection.type, texture))
      {
        projection_actions.push_back(action);
      }
    }
  }

  if (xf_state_manager.DidProjectionChange() || g_freelook_camera.GetController()->IsDirty() ||
      !projection_actions.empty() || m_projection_graphics_mod_change)
  {
    xf_state_manager.ResetProjection();
    m_projection_graphics_mod_change = !projection_actions.empty();

    auto corrected_matrix = LoadProjectionMatrix();

    GraphicsModActionData::Projection projection{&corrected_matrix};
    for (const auto& action : projection_actions)
    {
      action->OnProjection(&projection);
    }

    memcpy(constants.projection.data(), corrected_matrix.data.data(), 4 * sizeof(float4));
    dirty = true;
  }

  if (xf_state_manager.DidTexMatrixInfoChange())
  {
    xf_state_manager.ResetTexMatrixInfoChange();
    constants.xfmem_dualTexInfo = xfmem.dualTexTrans.enabled;
    for (size_t i = 0; i < std::size(xfmem.texMtxInfo); i++)
      constants.xfmem_pack1[i][0] = xfmem.texMtxInfo[i].hex;
    for (size_t i = 0; i < std::size(xfmem.postMtxInfo); i++)
      constants.xfmem_pack1[i][1] = xfmem.postMtxInfo[i].hex;

    dirty = true;
  }

  if (xf_state_manager.DidLightingConfigChange())
  {
    xf_state_manager.ResetLightingConfigChange();

    for (size_t i = 0; i < 2; i++)
    {
      constants.xfmem_pack1[i][2] = xfmem.color[i].hex;
      constants.xfmem_pack1[i][3] = xfmem.alpha[i].hex;
    }
    constants.xfmem_numColorChans = xfmem.numChan.numColorChans;
    dirty = true;
  }
}

void VertexShaderManager::TransformToClipSpace(const float* data, float* out, u32 MtxIdx)
{
  const float* world_matrix = &xfmem.posMatrices[(MtxIdx & 0x3f) * 4];

  // We use the projection matrix calculated by VertexShaderManager, because it
  // includes any free look transformations.
  // Make sure VertexShaderManager::SetConstants() has been called first.
  const float* proj_matrix = &m_projection_matrix[0];

  const float t[3] = {data[0] * world_matrix[0] + data[1] * world_matrix[1] +
                          data[2] * world_matrix[2] + world_matrix[3],
                      data[0] * world_matrix[4] + data[1] * world_matrix[5] +
                          data[2] * world_matrix[6] + world_matrix[7],
                      data[0] * world_matrix[8] + data[1] * world_matrix[9] +
                          data[2] * world_matrix[10] + world_matrix[11]};

  out[0] = t[0] * proj_matrix[0] + t[1] * proj_matrix[1] + t[2] * proj_matrix[2] + proj_matrix[3];
  out[1] = t[0] * proj_matrix[4] + t[1] * proj_matrix[5] + t[2] * proj_matrix[6] + proj_matrix[7];
  out[2] = t[0] * proj_matrix[8] + t[1] * proj_matrix[9] + t[2] * proj_matrix[10] + proj_matrix[11];
  out[3] =
      t[0] * proj_matrix[12] + t[1] * proj_matrix[13] + t[2] * proj_matrix[14] + proj_matrix[15];
}

void VertexShaderManager::DoState(PointerWrap& p)
{
  p.DoArray(m_projection_matrix);
  g_freelook_camera.DoState(p);

  p.Do(constants);

  if (p.IsReadMode())
  {
    dirty = true;
  }
}
