// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexShaderManager.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iterator>

#include "Core/System.h"
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
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"
#include "VideoCommon/VR.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/Statistics.h"

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

  if (g_ActiveConfig.bEnableVR)
    VR_UpdateHeadTrackingIfNeeded();

  if (g_ActiveConfig.bEnableVR && xfmem.projection.type == ProjectionType::Perspective)// && g_vulkan_context->GetDeviceInfo().multiview)
  {
    Common::Matrix44 head_position_matrix = Common::Matrix44::Identity();
    Common::Vec3 pos;
    if (g_ActiveConfig.bPositionTracking)
    {
      for (int i = 0; i < 3; ++i)
        pos.data[i] = g_head_tracking_position.data[i];// *UnitsPerMetre;
      head_position_matrix *= Common::Matrix44::Translate(pos);
    }

    Common::Matrix44 head_rotation_matrix = g_head_tracking_matrix;//g_freelook_camera.GetView();

    bool bFullscreenLayer = g_ActiveConfig.bHudFullscreen && xfmem.projection.type != ProjectionType::Perspective;
    bool bFlashing = false; // TODO: (debug_projNum - 1) == g_ActiveConfig.iSelectedLayer;
    bool bStuckToHead = false, bHide = false;
    int flipped_x = 1, flipped_y = 1, iTelescopeHack = -1;
    float fScaleHack = 1, fWidthHack = 1, fHeightHack = 1, fUpHack = 0, fRightHack = 0;

    float fLeftWidthHack = fWidthHack, fRightWidthHack = fWidthHack;
    float fLeftHeightHack = fHeightHack, fRightHeightHack = fHeightHack;
    bool bHideLeft = bHide, bHideRight = bHide, bNoForward = false;

    Common::Matrix44 head_view_matrix = Common::Matrix44::Identity();// g_freelook_camera.GetView();

      // --- THIS IS THE FINAL FIX ---
        // 1. Build the game's original projection matrix exactly as it would for non-VR.
        const auto& rawProjection = xfmem.projection.rawProjection;
        Common::Matrix44 game_projection_matrix;
        if (xfmem.projection.type == ProjectionType::Perspective)
        {
             // NOTE: We do NOT include fov_multiplier here. That is part of the head_view_matrix.
            game_projection_matrix.data[0] = rawProjection[0] * g_ActiveConfig.fAspectRatioHackW;
            game_projection_matrix.data[1] = 0.0f;
            game_projection_matrix.data[2] = rawProjection[1] * g_ActiveConfig.fAspectRatioHackW;
            game_projection_matrix.data[3] = 0.0f;
            game_projection_matrix.data[4] = 0.0f;
            game_projection_matrix.data[5] = rawProjection[2] * g_ActiveConfig.fAspectRatioHackH;
            game_projection_matrix.data[6] = rawProjection[3] * g_ActiveConfig.fAspectRatioHackH;
            game_projection_matrix.data[7] = 0.0f;
            game_projection_matrix.data[8] = 0.0f;
            game_projection_matrix.data[9] = 0.0f;
            game_projection_matrix.data[10] = rawProjection[4];
            game_projection_matrix.data[11] = rawProjection[5];
            game_projection_matrix.data[12] = 0.0f;
            game_projection_matrix.data[13] = 0.0f;
            game_projection_matrix.data[14] = -1.0f;
            game_projection_matrix.data[15] = 0.0f;
        }
        else // Orthographic
        {
            game_projection_matrix = Common::Matrix44::Identity(); // VR on ortho is undefined, but this prevents crashes
        }

        float zfar, znear, zNear3D, hfov, vfov;

        zfar = rawProjection[5] / rawProjection[4];
        znear = (1 + rawProjection[5]) / rawProjection[4];
        float zn2 = rawProjection[5] / (rawProjection[4] - 1);
        float zf2 = rawProjection[5] / (rawProjection[4] + 1);
        hfov = 2 * atan(1.0f / rawProjection[0]) * 180.0f / 3.1415926535f;
        vfov = 2 * atan(1.0f / rawProjection[2]) * 180.0f / 3.1415926535f;

        Common::Matrix44 proj_left, proj_right, hmd_left, hmd_right;
        proj_left = game_projection_matrix;
        proj_right = game_projection_matrix;

        VR_GetProjectionMatrices(hmd_left, hmd_right, znear, zfar);

        proj_left.data[0 * 4 + 0] =
            hmd_left.data[0 * 4 + 0] * MathUtil::Sign(proj_left.data[0 * 4 + 0]) * fLeftWidthHack;  // h fov
        proj_left.data[1 * 4 + 1] =
            hmd_left.data[1 * 4 + 1] * MathUtil::Sign(proj_left.data[1 * 4 + 1]) * fLeftHeightHack;  // v fov
        proj_left.data[0 * 4 + 2] =
            hmd_left.data[0 * 4 + 2] * MathUtil::Sign(proj_left.data[0 * 4 + 0]) - fRightHack;  // h off-axis
        proj_left.data[1 * 4 + 2] =
            hmd_left.data[1 * 4 + 2] * MathUtil::Sign(proj_left.data[1 * 4 + 1]) - fUpHack;  // v off-axis
        proj_right.data[0 * 4 + 0] =
            hmd_right.data[0 * 4 + 0] * MathUtil::Sign(proj_right.data[0 * 4 + 0]) * fRightWidthHack;
        proj_right.data[1 * 4 + 1] =
            hmd_right.data[1 * 4 + 1] * MathUtil::Sign(proj_right.data[1 * 4 + 1]) * fRightHeightHack;
        proj_right.data[0 * 4 + 2] =
            hmd_right.data[0 * 4 + 2] * MathUtil::Sign(proj_right.data[0 * 4 + 0]) - fRightHack;
        proj_right.data[1 * 4 + 2] =
            hmd_right.data[1 * 4 + 2] * MathUtil::Sign(proj_right.data[1 * 4 + 1]) - fUpHack;

        auto& system = Core::System::GetInstance();
        auto& geometry_shader_manager = system.GetGeometryShaderManager();
        geometry_shader_manager.constants.stereoparams[0] = 0;// proj_left.data[0 * 4 + 2];
        geometry_shader_manager.constants.stereoparams[1] = 0;// proj_right.data[0 * 4 + 2];
        geometry_shader_manager.constants.stereoparams[2] = 0;// proj_left.data[0 * 4 + 2];
        geometry_shader_manager.constants.stereoparams[3] = 0;// proj_right.data[0 * 4 + 2];
        /*if (g_backend_info.bSupportsGeometryShaders)
        {
          proj_left.data[0 * 4 + 2] = 0;
        }*/

        // 2. Get the eye-to-head VIEW matrices.
        Common::Matrix44 eye_to_head_left, eye_to_head_right;
        VR_GetEyeToHeadTransforms(&eye_to_head_left, &eye_to_head_right);
        
        //eye_to_head_left.data[2] = -20.2f;
        //eye_to_head_right.data[2] = -2.2f;
        
        // 3. Combine them in the correct order for post-multiplication: Head -> Eye -> Project
        Common::Matrix44 final_proj_left = head_view_matrix * eye_to_head_left * game_projection_matrix;
        Common::Matrix44 final_proj_right = head_view_matrix * eye_to_head_right * game_projection_matrix;
        // --- END OF FINAL FIX ---
        
        //final_proj_right.data[2] = 0.5f;
        proj_left *= head_position_matrix * head_rotation_matrix;
        proj_right *= head_position_matrix * head_rotation_matrix;

        memcpy(constants.projection[0].data(), proj_left.data.data(), sizeof(float4) * 4);
        memcpy(constants.projection[1].data(), proj_right.data.data(), sizeof(float4) * 4);

        //m_projection_matrix = final_proj_left.data;
        g_freelook_camera.GetController()->SetClean();
        return final_proj_left;
  }

  //if (false)//g_ActiveConfig.bEnableVR && xfmem.projection.type == ProjectionType::Perspective)
  //{
  //  // Transformations must be applied in the following order for VR:
  //  // HUD
  //  // camera position stabilisation
  //  // camera forward
  //  // camera pitch or rotation stabilisation
  //  // free look
  //  // leaning back
  //  // head position tracking
  //  // head rotation tracking
  //  // eye pos
  //  // projection

  //  ///////////////////////////////////////////////////////
  //  // First, identify any special layers and hacks

  //  bool bFullscreenLayer = g_ActiveConfig.bHudFullscreen && xfmem.projection.type != ProjectionType::Perspective;
  //  bool bFlashing = false; // TODO: (debug_projNum - 1) == g_ActiveConfig.iSelectedLayer;
  //  bool bStuckToHead = false, bHide = false;
  //  int flipped_x = 1, flipped_y = 1, iTelescopeHack = -1;
  //  float fScaleHack = 1, fWidthHack = 1, fHeightHack = 1, fUpHack = 0, fRightHack = 0;

  //  // TODO: if (g_ActiveConfig.iMetroidPrime || g_is_nes)
  //  //{
  //  //  GetMetroidPrimeValues(&bStuckToHead, &bFullscreenLayer, &bHide, &bFlashing, &fScaleHack,
  //  //                        &fWidthHack, &fHeightHack, &fUpHack, &fRightHack, &iTelescopeHack);
  //  //}

  //  // VR: in split-screen, only draw VR player TODO: fix offscreen to render to a separate texture in
  //  // VR
  //  //bHide = bHide ||
  //  //        (g_has_hmd && (g_viewport_type == VIEW_OFFSCREEN ||
  //  //                       (g_viewport_type >= VIEW_PLAYER_1 && g_viewport_type <= VIEW_PLAYER_4 &&
  //  //                        g_ActiveConfig.iVRPlayer != g_viewport_type - VIEW_PLAYER_1)));
  //  //// flash selected layer for debugging
  //  //bHide = bHide || (bFlashing && g_ActiveConfig.iFlashState > 5);
  //  //// hide skybox or everything to reduce motion sickness
  //  //bHide = bHide || (g_is_skybox && g_ActiveConfig.iMotionSicknessSkybox == 1) || g_vr_black_screen;

  //  // Split WidthHack and HeightHack into left and right versions for telescopes
  //  float fLeftWidthHack = fWidthHack, fRightWidthHack = fWidthHack;
  //  float fLeftHeightHack = fHeightHack, fRightHeightHack = fHeightHack;
  //  bool bHideLeft = bHide, bHideRight = bHide, bNoForward = false;
  //  // bool bTelescopeHUD = false;
  //  if (iTelescopeHack < 0 && g_ActiveConfig.iTelescopeEye &&
  //      vr_widest_3d_VFOV <= g_ActiveConfig.fTelescopeMaxFOV && vr_widest_3d_VFOV > 1 &&
  //      (g_ActiveConfig.fTelescopeMaxFOV <= g_ActiveConfig.fMinFOV ||
  //       (g_ActiveConfig.fTelescopeMaxFOV > g_ActiveConfig.fMinFOV &&
  //        vr_widest_3d_VFOV > g_ActiveConfig.fMinFOV)))
  //    iTelescopeHack = g_ActiveConfig.iTelescopeEye;
  //  if (g_has_hmd && iTelescopeHack > 0)
  //  {
  //    bNoForward = true;
  //    // Calculate telescope scale
  //    float hmd_halftan, telescope_scale;
  //    VR_GetProjectionHalfTan(hmd_halftan);
  //    telescope_scale = fabs(hmd_halftan / tan(DEGREES_TO_RADIANS(vr_widest_3d_VFOV) / 2));
  //    if (iTelescopeHack & 1)
  //    {
  //      fLeftWidthHack *= telescope_scale;
  //      fLeftHeightHack *= telescope_scale;
  //      bHideLeft = false;
  //    }
  //    if (iTelescopeHack & 2)
  //    {
  //      fRightWidthHack *= telescope_scale;
  //      fRightHeightHack *= telescope_scale;
  //      bHideRight = false;
  //    }
  //  }

  //  Common::Matrix44 game_projection_matrix;
  //  if (xfmem.projection.type == ProjectionType::Perspective)
  //  {
  //   /* u32 vr_width, vr_height;
  //    VR_GetRecommendedRenderTargetSize(&vr_width, &vr_height);
  //    u32 vr_aspect = vr_width / vr_height;
  //    u32 efb_aspect = g_framebuffer_manager->GetEFBFramebuffer()->GetWidth() / g_framebuffer_manager->GetEFBFramebuffer()->GetHeight();
  //    u32 aspect_correction = vr_aspect / efb_aspect;*/

  //    game_projection_matrix.data[0] = rawProjection[0]  *g_ActiveConfig.fAspectRatioHackW;
  //    game_projection_matrix.data[1] = 0.0f;
  //    game_projection_matrix.data[2] = rawProjection[1]  *g_ActiveConfig.fAspectRatioHackW;
  //    game_projection_matrix.data[3] = 0.0f;
  //    game_projection_matrix.data[4] = 0.0f;
  //    game_projection_matrix.data[5] = rawProjection[2]  *g_ActiveConfig.fAspectRatioHackH;
  //    game_projection_matrix.data[6] = rawProjection[3] *g_ActiveConfig.fAspectRatioHackH;
  //    game_projection_matrix.data[7] = 0.0f;
  //    game_projection_matrix.data[8] = 0.0f;
  //    game_projection_matrix.data[9] = 0.0f;
  //    game_projection_matrix.data[10] = rawProjection[4];
  //    game_projection_matrix.data[11] = rawProjection[5];
  //    game_projection_matrix.data[12] = 0.0f;
  //    game_projection_matrix.data[13] = 0.0f;
  //    game_projection_matrix.data[14] = -1.0f;
  //    game_projection_matrix.data[15] = 0.0f;
  //  }
  //  else // Orthographic
  //  {
  //      game_projection_matrix = Common::Matrix44::Identity(); // VR on ortho is undefined, but this prevents crashes
  //  }

  //  float UnitsPerMetre = g_ActiveConfig.fUnitsPerMetre * fScaleHack / g_ActiveConfig.fScale;

  //  bHide = bHide && (bFlashing || (g_has_hmd && g_ActiveConfig.bEnableVR));

  //  if (bHide)
  //  {
  //    // If we are supposed to hide the layer, zero out the projection matrix
  //    return Common::Matrix44::Zero();
  //  }
  //  // don't do anything fancy for rendering to a texture
  //  // render exactly as we are told, and in mono
  //  else if (g_viewport_type == VIEW_RENDER_TO_TEXTURE)
  //  {
  //    return game_projection_matrix;
  //  }
  //  else if (bFullscreenLayer)
  //  {
  //    Common::Matrix44 projMtx, scale_matrix, correctedMtx;
  //    projMtx = game_projection_matrix;

  //    projMtx.data[0] *= fWidthHack;
  //    projMtx.data[5] *= fHeightHack;
  //    projMtx.data[3] += fRightHack;
  //    projMtx.data[7] += fUpHack;

  //    scale_matrix = Common::Matrix44::Identity();
  //    correctedMtx = projMtx * scale_matrix;

  //    return correctedMtx;
  //  }
  //  // VR HMD 3D projection matrix, needs to include head-tracking
  //  else
  //  {
  //    // near clipping plane in game units
  //    float zfar, znear, zNear3D, hfov, vfov;

  //    // if the camera is zoomed in so much that the action only fills a tiny part of your FOV,
  //    // we need to move the camera forwards until objects at AimDistance fill the minimum FOV.
  //    float zoom_forward = 0.0f;
  //    if (vr_widest_3d_HFOV <= g_ActiveConfig.fMinFOV && vr_widest_3d_HFOV > 0 && iTelescopeHack <= 0)
  //    {
  //      zoom_forward = g_ActiveConfig.fAimDistance *
  //                     tanf(DEGREES_TO_RADIANS(g_ActiveConfig.fMinFOV) / 2) /
  //                     tanf(DEGREES_TO_RADIANS(vr_widest_3d_HFOV) / 2);
  //      zoom_forward -= g_ActiveConfig.fAimDistance;
  //    }

  //    // Real 3D scene
  //    if (true)//xfmem.projection.type == ProjectionType::Perspective && g_viewport_type != VIEW_HUD_ELEMENT &&
  //        //g_viewport_type != VIEW_OFFSCREEN)
  //    {
  //      zfar = rawProjection[5] / rawProjection[4];
  //      znear = (1 + rawProjection[5]) / rawProjection[4];
  //      float zn2 = rawProjection[5] / (rawProjection[4] - 1);
  //      float zf2 = rawProjection[5] / (rawProjection[4] + 1);
  //      hfov = 2 * atan(1.0f / rawProjection[0]) * 180.0f / 3.1415926535f;
  //      vfov = 2 * atan(1.0f / rawProjection[2]) * 180.0f / 3.1415926535f;
  //      
  //      if (!g_vr_had_3D_already)
  //      {
  //        CheckOrientationConstants();
  //        g_vr_had_3D_already = true;
  //      }
  //    }
  //    else
  //    {
  //      if (vr_widest_3d_HFOV > 0)
  //      {
  //        znear = vr_widest_3d_zNear;
  //        zfar = vr_widest_3d_zFar;
  //        if (zoom_forward != 0)
  //        {
  //          hfov = g_ActiveConfig.fMinFOV;
  //          vfov = g_ActiveConfig.fMinFOV * vr_widest_3d_VFOV / vr_widest_3d_HFOV;
  //        }
  //        else
  //        {
  //          hfov = vr_widest_3d_HFOV;
  //          vfov = vr_widest_3d_VFOV;
  //        }
  //      }
  //      else
  //      {
  //        znear = 0.2f * UnitsPerMetre * 20;  // 50cm
  //        zfar = 40 * UnitsPerMetre;          // 40m
  //        hfov = 70;                          // 70 degrees
  //        if (g_is_nes)
  //          vfov =
  //              180.0f / 3.14159f * 2 * atanf(tanf((hfov * 3.14159f / 180.0f) / 2) * 1.0f / 1.175f);
  //        else if (g_ActiveConfig.aspect_mode == AspectMode::ForceWide)
  //          vfov =
  //              180.0f / 3.14159f * 2 * atanf(tanf((hfov * 3.14159f / 180.0f) / 2) * 9.0f /
  //                                            16.0f);  // 2D screen is meant to be 16:9 aspect ratio
  //        else
  //          vfov = 180.0f / 3.14159f * 2 * atanf(tanf((hfov * 3.14159f / 180.0f) / 2) * 3.0f /
  //                                               4.0f);  //  2D screen is meant to be 4:3 aspect ratio
  //      }
  //      zNear3D = znear;
  //      znear /= 40.0f;
  //      game_projection_matrix.data[0] = 1.0f;
  //      game_projection_matrix.data[1] = 0.0f;
  //      game_projection_matrix.data[2] = 0.0f;
  //      game_projection_matrix.data[3] = 0.0f;
  //      game_projection_matrix.data[4] = 0.0f;
  //      game_projection_matrix.data[5] = 1.0f;
  //      game_projection_matrix.data[6] = 0.0f;
  //      game_projection_matrix.data[7] = 0.0f;
  //      game_projection_matrix.data[8] = 0.0f;
  //      game_projection_matrix.data[9] = 0.0f;
  //      game_projection_matrix.data[10] = -znear / (zfar - znear);
  //      game_projection_matrix.data[11] = -zfar * znear / (zfar - znear);
  //      game_projection_matrix.data[12] = 0.0f;
  //      game_projection_matrix.data[13] = 0.0f;
  //      game_projection_matrix.data[14] = -1.0f;
  //      game_projection_matrix.data[15] = 0.0f;
  //    }

  //    Common::Matrix44 proj_left, proj_right, hmd_left, hmd_right;
  //    proj_left = game_projection_matrix;
  //    proj_right = game_projection_matrix;

  //    VR_GetProjectionMatrices(hmd_left, hmd_right, znear, zfar);

  //    proj_left.data[0 * 4 + 0] =
  //        hmd_left.data[0 * 4 + 0] * MathUtil::Sign(proj_left.data[0 * 4 + 0]) * fLeftWidthHack;  // h fov
  //    proj_left.data[1 * 4 + 1] =
  //        hmd_left.data[1 * 4 + 1] * MathUtil::Sign(proj_left.data[1 * 4 + 1]) * fLeftHeightHack;  // v fov
  //    proj_left.data[0 * 4 + 2] =
  //        hmd_left.data[0 * 4 + 2] * MathUtil::Sign(proj_left.data[0 * 4 + 0]) - fRightHack;  // h off-axis
  //    proj_left.data[1 * 4 + 2] =
  //        hmd_left.data[1 * 4 + 2] * MathUtil::Sign(proj_left.data[1 * 4 + 1]) - fUpHack;  // v off-axis
  //    proj_right.data[0 * 4 + 0] =
  //        hmd_right.data[0 * 4 + 0] * MathUtil::Sign(proj_right.data[0 * 4 + 0]) * fRightWidthHack;
  //    proj_right.data[1 * 4 + 1] =
  //        hmd_right.data[1 * 4 + 1] * MathUtil::Sign(proj_right.data[1 * 4 + 1]) * fRightHeightHack;
  //    proj_right.data[0 * 4 + 2] =
  //        hmd_right.data[0 * 4 + 2] * MathUtil::Sign(proj_right.data[0 * 4 + 0]) - fRightHack;
  //    proj_right.data[1 * 4 + 2] =
  //        hmd_right.data[1 * 4 + 2] * MathUtil::Sign(proj_right.data[1 * 4 + 1]) - fUpHack;
  //    
  //    auto& system = Core::System::GetInstance();
  //    auto& geometry_shader_manager = system.GetGeometryShaderManager();
  //    geometry_shader_manager.constants.stereoparams[0] = 0;// proj_left.data[0 * 4 + 2];
  //    geometry_shader_manager.constants.stereoparams[1] = 0;// proj_right.data[0 * 4 + 2];
  //    geometry_shader_manager.constants.stereoparams[2] = 0;// proj_left.data[0 * 4 + 2];
  //    geometry_shader_manager.constants.stereoparams[3] = 0;// proj_right.data[0 * 4 + 2];
  //    /*if (g_backend_info.bSupportsGeometryShaders)
  //    {
  //      proj_left.data[0 * 4 + 2] = 0;
  //    }*/

  //    Common::Matrix44 rotation_matrix;
  //    Common::Matrix44 lean_back_matrix;
  //    Common::Matrix44 camera_pitch_matrix;
		//	rotation_matrix = Common::Matrix44::Identity();
		//	lean_back_matrix = Common::Matrix44::Identity();
		//	camera_pitch_matrix = Common::Matrix44::Identity();
  //    if (!bStuckToHead)
  //    {
  //      if (g_ActiveConfig.bOrientationTracking)
  //      {
  //        //VR_UpdateHeadTrackingIfNeeded();
  //        rotation_matrix = g_head_tracking_matrix;
  //      }
  //      else
  //      {
  //        rotation_matrix = Common::Matrix44::Identity();
  //      }

  //      Common::Matrix33 pitch_matrix33 = Common::Matrix33::Identity();
  //      float extra_pitch = -g_ActiveConfig.fLeanBackAngle;
  //      pitch_matrix33 *= Common::Matrix33::RotateX(-DEGREES_TO_RADIANS(extra_pitch));
  //      lean_back_matrix = Common::Matrix44::FromMatrix33(pitch_matrix33);

  //      if ((g_ActiveConfig.bStabilizePitch || g_ActiveConfig.bStabilizeRoll ||
  //           g_ActiveConfig.bStabilizeYaw) &&
  //          g_ActiveConfig.bCanReadCameraAngles &&
  //          (g_ActiveConfig.iMotionSicknessSkybox != 2 || !g_is_skybox))
  //      {
  //        if (!g_ActiveConfig.bStabilizePitch)
  //        {
  //          Common::Matrix44 user_pitch44;
  //          Common::Matrix44 roll_and_yaw_matrix;
  //          if (xfmem.projection.type == ProjectionType::Perspective || vr_widest_3d_HFOV > 0)
  //            extra_pitch = g_ActiveConfig.fCameraPitch;
  //          else
  //            extra_pitch = g_ActiveConfig.fScreenPitch;
  //          pitch_matrix33 *= Common::Matrix33::RotateX(-DEGREES_TO_RADIANS(extra_pitch));
  //          user_pitch44 = Common::Matrix44::FromMatrix33(pitch_matrix33);
  //          roll_and_yaw_matrix = g_game_camera_rotmat;
  //          camera_pitch_matrix = user_pitch44 * roll_and_yaw_matrix;
  //        }
  //        else
  //        {
  //          camera_pitch_matrix = g_game_camera_rotmat;
  //        }
  //      }
  //      else
  //      {
  //        if (xfmem.projection.type == ProjectionType::Perspective || vr_widest_3d_HFOV > 0)
  //          extra_pitch = g_ActiveConfig.fCameraPitch;
  //        else
  //          extra_pitch = g_ActiveConfig.fScreenPitch;
  //        pitch_matrix33 *= Common::Matrix33::RotateX(-DEGREES_TO_RADIANS(extra_pitch));
  //        camera_pitch_matrix = Common::Matrix44::FromMatrix33(pitch_matrix33);
  //      }
  //    }

  //    if (xfmem.projection.type == ProjectionType::Perspective)
  //    {
  //      if (rawProjection[0] < 0)
  //        flipped_x = -1;
  //      if (rawProjection[2] < 0)
  //        flipped_y = -1;
  //    }

  //    Common::Matrix44 head_position_matrix, free_look_matrix, camera_forward_matrix, camera_position_matrix;
		//	head_position_matrix = Common::Matrix44::Identity();
		//	free_look_matrix = Common::Matrix44::Identity();
		//	camera_position_matrix = Common::Matrix44::Identity();
  //    if (!bStuckToHead && !g_is_skybox)
  //    {
  //      Common::Vec3 pos;
  //      if (g_ActiveConfig.bPositionTracking)
  //      {
  //        for (int i = 0; i < 3; ++i)
  //          pos.data[i] = g_head_tracking_position.data[i] * UnitsPerMetre;
  //        head_position_matrix.Translate(pos);
  //      }
  //      else
  //      {
  //        head_position_matrix = Common::Matrix44::Identity();
  //      }

  //      for (int i = 0; i < 3; ++i)
  //        pos.data[i] = g_freelook_camera.GetController()->GetView().data[i * 4 + 3] * UnitsPerMetre;
  //      free_look_matrix *= Common::Matrix44::Translate(pos);

  //      if (g_ActiveConfig.bStabilizeX || g_ActiveConfig.bStabilizeY || g_ActiveConfig.bStabilizeZ)
  //      {
  //        for (int i = 0; i < 3; ++i)
  //          pos.data[i] = -g_game_camera_pos.data[i] * UnitsPerMetre;
  //        camera_position_matrix *= Common::Matrix44::Translate(pos);
  //      }
  //      else
  //      {
  //        camera_position_matrix = Common::Matrix44::Identity();
  //      }
  //    }

  //    Common::Matrix44 look_matrix;
  //    if (xfmem.projection.type == ProjectionType::Perspective && g_viewport_type != VIEW_HUD_ELEMENT &&
  //        g_viewport_type != VIEW_OFFSCREEN)
  //    {
  //      if (bNoForward || g_is_skybox || bStuckToHead)
  //      {
  //        camera_forward_matrix = Common::Matrix44::Identity();
  //      }
  //      else
  //      {
  //        Common::Vec3 pos;
  //        pos.data[0] = 0;
  //        pos.data[1] = 0;
  //        pos.data[2] = (g_ActiveConfig.fCameraForward + zoom_forward) * UnitsPerMetre;
  //        camera_forward_matrix *= Common::Matrix44::Translate(pos);
  //      }
  //      //look_matrix = camera_forward_matrix * camera_position_matrix * camera_pitch_matrix *
  //      //              free_look_matrix * lean_back_matrix * head_position_matrix * rotation_matrix;
  //      look_matrix = head_position_matrix * rotation_matrix;
  //    }
  //    else
  //    {
  //      float HudWidth, HudHeight, HudThickness, HudDistance, HudUp, CameraForward, AimDistance;

  //      if (vr_widest_3d_HFOV <= 0)
  //      {
  //        HudThickness = g_ActiveConfig.fScreenThickness * UnitsPerMetre;
  //        HudDistance = g_ActiveConfig.fScreenDistance * UnitsPerMetre;
  //        HudHeight = g_ActiveConfig.fScreenHeight * UnitsPerMetre;
  //        if (g_is_nes)
  //          HudWidth = HudHeight * 1.175f;
  //        else if (g_ActiveConfig.aspect_mode == AspectMode::ForceWide)
  //          HudWidth = HudHeight * (float)16 / 9;
  //        else
  //          HudWidth = HudHeight * (float)4 / 3;
  //        CameraForward = 0;
  //        HudUp = g_ActiveConfig.fScreenUp * UnitsPerMetre;
  //        AimDistance = HudDistance;
  //      }
  //      else
  //      {
  //        const float MinHudDistance = 0.28f, MaxHudDistance = 3.00f;
  //        float HUDScale = g_ActiveConfig.fScale;
  //        if (HUDScale < 1.0f && g_ActiveConfig.fHudDistance >= MinHudDistance &&
  //            g_ActiveConfig.fHudDistance * HUDScale < MinHudDistance)
  //          HUDScale = MinHudDistance / g_ActiveConfig.fHudDistance;
  //        else if (HUDScale > 1.0f && g_ActiveConfig.fHudDistance <= MaxHudDistance &&
  //                 g_ActiveConfig.fHudDistance * HUDScale > MaxHudDistance)
  //          HUDScale = MaxHudDistance / g_ActiveConfig.fHudDistance;

  //        HudThickness = g_ActiveConfig.fHudThickness * HUDScale * UnitsPerMetre;
  //        HudDistance = g_ActiveConfig.fHudDistance * HUDScale * UnitsPerMetre;
  //        HudUp = 0;
  //        if (bNoForward)
  //          CameraForward = 0;
  //        else
  //          CameraForward = (g_ActiveConfig.fCameraForward + zoom_forward) * g_ActiveConfig.fScale * UnitsPerMetre;
  //        AimDistance = g_ActiveConfig.fAimDistance * g_ActiveConfig.fScale * UnitsPerMetre;
  //        if (AimDistance <= 0)
  //          AimDistance = HudDistance;
  //        HudWidth = 2.0f * tanf(DEGREES_TO_RADIANS(hfov / 2.0f)) * HudDistance * (AimDistance + CameraForward) / AimDistance;
  //        HudHeight = 2.0f * tanf(DEGREES_TO_RADIANS(vfov / 2.0f)) * HudDistance * (AimDistance + CameraForward) / AimDistance;
  //      }

  //      Common::Vec3 scale, position;
  //      Common::Vec2 viewport_scale = {1.0f, 1.0f};
  //      Common::Vec2 viewport_offset = {0.0f, 0.0f};
  //      
  //      if (xfmem.projection.type == ProjectionType::Perspective)
  //      {
  //        float left2D = -(rawProjection[1] + 1) / rawProjection[0];
  //        float right2D = left2D + 2 / rawProjection[0];
  //        float bottom2D = -(rawProjection[3] + 1) / rawProjection[2];
  //        float top2D = bottom2D + 2 / rawProjection[2];
  //        float zFar2D = rawProjection[5] / rawProjection[4];
  //        float zNear2D = zFar2D * rawProjection[4] / (rawProjection[4] - 1);
  //        float zObj = zNear2D + (zFar2D - zNear2D) * g_ActiveConfig.fHud3DCloser;
  //        left2D *= zObj;
  //        right2D *= zObj;
  //        bottom2D *= zObj;
  //        top2D *= zObj;

  //        if (rawProjection[0] == 0 || right2D == left2D)
  //          scale.x = 0;
  //        else
  //          scale.x = viewport_scale.x * HudWidth / (right2D - left2D);
  //        if (rawProjection[2] == 0 || top2D == bottom2D)
  //          scale.y = 0;
  //        else
  //          scale.y = viewport_scale.y * HudHeight / (top2D - bottom2D);
  //        if (rawProjection[4] == 0 || zFar2D == zNear2D)
  //          scale.z = scale.x;
  //        else
  //          scale.z = scale.x;
  //        position.x = scale.x * (-(right2D + left2D) / 2.0f) + viewport_offset.x * HudWidth;
  //        position.y = scale.y * (-(top2D + bottom2D) / 2.0f) + viewport_offset.y * HudHeight + HudUp;
  //        if (vr_widest_3d_HFOV <= 0)
  //          position.z = scale.z * zObj - HudDistance;
  //        else
  //          position.z = scale.z * zObj - HudDistance;
  //      }
  //      else
  //      {
  //        float left2D = -(rawProjection[1] + 1) / rawProjection[0];
  //        float right2D = left2D + 2 / rawProjection[0];
  //        float bottom2D = -(rawProjection[3] + 1) / rawProjection[2];
  //        float top2D = bottom2D + 2 / rawProjection[2];
  //        float zFar2D, zNear2D;
  //        zFar2D = rawProjection[5] / rawProjection[4];
  //        zNear2D = (1 + rawProjection[4] * zFar2D) / rawProjection[4];

  //        if (rawProjection[0] == 0 || right2D == left2D)
  //          scale.x = 0;
  //        else
  //          scale.x = viewport_scale.x * HudWidth / (right2D - left2D);
  //        if (rawProjection[2] == 0 || top2D == bottom2D)
  //          scale.y = 0;
  //        else
  //          scale.y = viewport_scale.y * HudHeight / (top2D - bottom2D);
  //        if (rawProjection[4] == 0 || zFar2D == zNear2D)
  //          scale.z = 0;
  //        else
  //          scale.z = HudThickness / (zFar2D - zNear2D);
  //        position.x = scale.x * (-(right2D + left2D) / 2.0f) + viewport_offset.x * HudWidth;
  //        position.y = scale.y * (-(top2D + bottom2D) / 2.0f) + viewport_offset.y * HudHeight + HudUp;
  //        if (vr_widest_3d_HFOV <= 0)
  //          position.z = -HudDistance;
  //        else
  //          position.z = -HudDistance;
  //      }
  //      
  //      position.x += (g_ActiveConfig.fHudDespPosition0 * g_ActiveConfig.fUnitsPerMetre);
  //      position.y += (g_ActiveConfig.fHudDespPosition1 * g_ActiveConfig.fUnitsPerMetre);
  //      position.z += (g_ActiveConfig.fHudDespPosition2 * g_ActiveConfig.fUnitsPerMetre);
  //      
  //      Common::Matrix44 hud_matrix;
  //      // TODO: g_ActiveConfig.matrixHudrot;
  //      hud_matrix = Common::Matrix44::Identity();

  //      Common::Matrix44 scale_matrix, position_matrix;
		//		scale_matrix = Common::Matrix44::Identity();
		//		position_matrix = Common::Matrix44::Identity();
  //      //scale_matrix *= Common::Matrix44::Scale(scale);
  //      //position_matrix *= Common::Matrix44::Translate(position);
  //      
  //      //look_matrix = scale_matrix * hud_matrix * position_matrix * camera_position_matrix * camera_pitch_matrix *
  //      //  free_look_matrix * lean_back_matrix * head_position_matrix * rotation_matrix;
  //      look_matrix = head_position_matrix * rotation_matrix;
  //    }

  //    Common::Matrix44 eye_pos_matrix_left, eye_pos_matrix_right;
  //    Common::Vec3 posLeft = {0, 0, 0};
  //    Common::Vec3 posRight = {0, 0, 0};
  //    if (!g_is_skybox)
  //    {
  //      VR_GetEyePos(posLeft.data.data(), posRight.data.data());
  //      posLeft = posLeft * UnitsPerMetre;
  //      posRight = posRight * UnitsPerMetre;
  //    }

  //    Common::Matrix44 view_matrix_left, view_matrix_right;
  //    if (g_backend_info.bSupportsGeometryShaders)
  //    {
  //      view_matrix_left = look_matrix;
  //      view_matrix_right = look_matrix;
  //    }
  //    else
  //    {
  //      eye_pos_matrix_left *= Common::Matrix44::Translate(posLeft);
  //      eye_pos_matrix_right *= Common::Matrix44::Translate(posRight);
  //      view_matrix_left = eye_pos_matrix_left * look_matrix;
  //      view_matrix_right = eye_pos_matrix_right * look_matrix;
  //    }
  //    Common::Matrix44 final_matrix_left, final_matrix_right;
	 // final_matrix_left = proj_left * view_matrix_left;
	 // final_matrix_right = proj_right * view_matrix_right;
  //    if (flipped_x < 0)
  //    {
  //      final_matrix_left.data[1] *= -1;
  //      final_matrix_left.data[2] *= -1;
  //      final_matrix_left.data[3] *= -1;
  //      //geometry_shader_manager.constants.stereoparams[2] *= -1;
  //      final_matrix_left.data[4] *= -1;
  //      final_matrix_left.data[8] *= -1;
  //      final_matrix_left.data[12] *= -1;
  //      final_matrix_right.data[1] *= -1;
  //      final_matrix_right.data[2] *= -1;
  //      final_matrix_right.data[3] *= -1;
  //      //geometry_shader_manager.constants.stereoparams[3] *= -1;
  //      final_matrix_right.data[4] *= -1;
  //      final_matrix_right.data[8] *= -1;
  //      final_matrix_right.data[12] *= -1;
  //      //geometry_shader_manager.constants.stereoparams[0] *= -1;
  //      //geometry_shader_manager.constants.stereoparams[1] *= -1;
  //    }
  //    if (flipped_y < 0)
  //    {
  //      final_matrix_left.data[1] *= -1;
  //      final_matrix_left.data[4] *= -1;
  //      final_matrix_left.data[6] *= -1;
  //      final_matrix_left.data[7] *= -1;
  //      final_matrix_left.data[9] *= -1;
  //      final_matrix_left.data[13] *= -1;
  //      final_matrix_right.data[1] *= -1;
  //      final_matrix_right.data[4] *= -1;
  //      final_matrix_right.data[6] *= -1;
  //      final_matrix_right.data[7] *= -1;
  //      final_matrix_right.data[9] *= -1;
  //      final_matrix_right.data[13] *= -1;
  //    }

  //    if (bHideLeft && (bHideRight || g_ActiveConfig.stereo_mode == StereoMode::Off))
  //      final_matrix_left = Common::Matrix44::Zero();
  //    if (bHideRight)
  //      final_matrix_right = Common::Matrix44::Zero();

  //    memcpy(constants.projection[0].data(), final_matrix_left.data.data(), sizeof(float4) * 4);
  //    memcpy(constants.projection[1].data(), final_matrix_right.data.data(), sizeof(float4) * 4);
  //    
  //    /*if (g_ActiveConfig.stereo_mode == StereoMode::OpenVR)
  //    {
  //      geometry_shader_manager.constants.stereoparams[0] *= posLeft[0];
  //      geometry_shader_manager.constants.stereoparams[1] *= posRight[0];
  //    }
  //    else
  //    {
  //      geometry_shader_manager.constants.stereoparams[0] =
  //          geometry_shader_manager.constants.stereoparams[1] = 0;
  //    }*/
  //    
  //    g_freelook_camera.GetController()->SetClean();
  //    return final_matrix_left;
  //  }
  //}

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

    // If FreeLook is active for a perspective scene, we override the entire
    // view-projection matrix with the one required for VR.
    if (g_freelook_camera.IsActive() && xfmem.projection.type == ProjectionType::Perspective)
    {
        corrected_matrix *= g_freelook_camera.GetView();
    }
    
    // The freelook controller state is now incorporated into the matrix.
    g_freelook_camera.GetController()->SetClean();
    
    // For non-multiview rendering, just load the same matrix into both slots.
    memcpy(constants.projection[0].data(), corrected_matrix.data.data(), sizeof(float4) * 4);
    memcpy(constants.projection[1].data(), corrected_matrix.data.data(), sizeof(float4) * 4);

    // Return the final matrix to be uploaded to the shader.
    return corrected_matrix;

}

void VertexShaderManager::SetProjectionMatrix(XFStateManager& xf_state_manager)
{
  if (xf_state_manager.DidProjectionChange() || g_freelook_camera.GetController()->IsDirty())
  {
    xf_state_manager.ResetProjection();

    // This call now handles both VR and non-VR cases and uploads the correct
    // matrix/matrices to the constants buffer.
    LoadProjectionMatrix();
  }
}

void VertexShaderManager::SetViewportType(const Viewport& v)
{
  // VR
  g_old_viewport_type = g_viewport_type;
  float left, top, width, height;
  left = v.xOrig - v.wd;
  top = v.yOrig - v.ht;
  width = 2 * v.wd;
  height = 2 * v.ht;
  float screen_width = (float)g_final_screen_region.GetWidth();
  float screen_height = (float)g_final_screen_region.GetHeight();
  float min_screen_width = 0.90f * screen_width;
  float min_screen_height = 0.90f * screen_height;
  float max_top = screen_height - min_screen_height;
  float max_left = screen_width - min_screen_width;

  // Power of two square viewport in the corner of the screen means we are rendering to a texture,
  // usually a shadow texture from the POV of the light, that will be drawn multitextured onto 3D
  // objects.
  // Note this is a temporary rendering to the backbuffer that will be cleared or overwritten after
  // reading.
  // Twilighlight Princess GC uses square but non-power-of-2 textures: 216x216 and 384x384
  // Metroid Prime 2 uses square textures in the bottom left corner but screen_height is wrong.
  // So the relaxed rule is: square texture on any screen edge with size a multiple of 8
  // Bad Boys 2 and 007 Everything or Nothing use 512x512 viewport and 512x512 screen size for
  // non-render-targets
  // if (width == height
  //	&& (width == 32 || width == 64 || width == 128 || width == 256)
  //	&& ((left == 0 && top == 0) || (left == 0 && top == screen_height - height)
  //	|| (left == screen_width - width && top == 0) || (left == screen_width - width && top ==
  // screen_height - height)))
  if (width == height && (width == 1 || width == 2 || width == 4 || ((int)width % 8 == 0)) &&
      (left == 0 || top == 0 || top == screen_height - height || left == screen_width - width) &&
      !(width == 512 && screen_width == 512 && screen_height == 512))
  {
    g_viewport_type = VIEW_RENDER_TO_TEXTURE;
  }
  // NES games render to the EFB copy and end up being projected twice, but this is now handled
  // elsewhere
  // else if (g_is_nes && width == 512 && height == 228 && left == 0 && top == 0)
  //{
  //	g_viewport_type = VIEW_RENDER_TO_TEXTURE;
  //}
  // Zelda Twilight Princess uses this strange viewport for rendering the Map Screen's coloured map
  // highlights to a texture.
  // I don't think it will break any other games, because it makes little sense as a real viewport.
  else if (width == 457 && height == 341 && left == 0 && top == 0)
  {
    g_viewport_type = VIEW_RENDER_TO_TEXTURE;
  }
  // Full width could mean fullscreen, letterboxed, or splitscreen top and bottom.
  else if (width >= min_screen_width)
  {
    if (left <= max_left)
    {
      if (height >= min_screen_height)
      {
        if (top <= max_top)
        {
          if (width == screen_width && height == screen_height)
            g_viewport_type = VIEW_FULLSCREEN;
          else
            g_viewport_type = VIEW_LETTERBOXED;
        }
        else
        {
          g_viewport_type = VIEW_OFFSCREEN;
        }
      }
      else if (height >= min_screen_height / 2 && height <= screen_height / 2)
      {
        if (top <= max_top)
        {
          // 2 Player Split Screen, top half
          g_viewport_type = VIEW_PLAYER_1;
        }
        else if (top >= height && top <= height + max_top)
        {
          // 2 Player Split Screen, bottom half
            g_viewport_type = VIEW_PLAYER_2;
        }
        else
        {
          // band across middle of screen
          g_viewport_type = VIEW_LETTERBOXED;
        }
      }
      else
      {
        // band across middle of screen
        g_viewport_type = VIEW_LETTERBOXED;
        // setting this to HUD element breaks morphball mode in Metroid Prime
        //     HUD element (0,45) 640x358; near=0.999 (1.67604e+007), far=1 (1.67772e+007)
      }
    }
    else
    {
      g_viewport_type = VIEW_OFFSCREEN;
    }
  }
  else if (height >= min_screen_height)
  {
    if (top <= max_top)
    {
      if (width >= min_screen_width / 2)
      {
        if (left <= max_left)
        {
          // 2 Player Split Screen, left half
          g_viewport_type = VIEW_PLAYER_1;
        }
        else if (left >= width)
        {
          // 2 Player Split Screen, right half
            g_viewport_type = VIEW_PLAYER_2;
        }
        else
        {
          // column down middle of screen
          g_viewport_type = VIEW_HUD_ELEMENT;
        }
      }
      else
      {
        // column down middle of screen
        g_viewport_type = VIEW_LETTERBOXED;
      }
    }
    else
    {
      g_viewport_type = VIEW_OFFSCREEN;
    }
  }
  // Quadrants
  else if (width >= (min_screen_width / 2) && height >= (min_screen_height / 2) &&
           width <= (screen_width / 2) && height <= (screen_height / 2))
  {
    // top left
    if (left <= max_left && top <= max_top)
    {
      g_viewport_type = VIEW_PLAYER_1;
    }
    // top right
    else if (left >= width && top <= max_top)
    {
      g_viewport_type = VIEW_PLAYER_2;
    }
    // bottom left
    else if (left <= max_left && top >= height)
    {
      g_viewport_type = VIEW_PLAYER_3;
    }
    // bottom right
    else if (left >= width && top >= height)
    {
      g_viewport_type = VIEW_PLAYER_4;
    }
    else
    {
      g_viewport_type = VIEW_HUD_ELEMENT;
    }
  }
  else if (left >= g_final_screen_region.right || top >= g_final_screen_region.bottom ||
           left + width <= g_final_screen_region.left || top + height <= g_final_screen_region.top)
  {
    g_viewport_type = VIEW_OFFSCREEN;
  }
  else
  {
    g_viewport_type = VIEW_HUD_ELEMENT;
  }
  if (g_viewport_type == VIEW_FULLSCREEN || g_viewport_type == VIEW_LETTERBOXED ||
      (g_viewport_type >= VIEW_PLAYER_1 && g_viewport_type <= VIEW_PLAYER_4))
  {
    // check if it is a skybox
    float znear = (v.farZ - v.zRange) / 16777216.0f;
    float zfar = v.farZ / 16777216.0f;

    if (znear >= 0.99f && zfar >= 0.999f)
      g_is_skybox = true;
    else
      g_is_skybox = false;
  }
  else
  {
    g_is_skybox = false;
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
    if (g_ActiveConfig.bEnableVR)
        CheckOrientationConstants();
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

    if (g_ActiveConfig.bEnableVR)
    {
        SetViewportType(xfmem.viewport);
        if (g_ActiveConfig.bDetectSkybox && !g_is_skybox)
            CheckSkybox();
    }

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

    //memcpy(constants.projection.data(), corrected_matrix.data.data(), 4 * sizeof(float4));
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

void VertexShaderManager::CheckOrientationConstants()
{
#define sqr(a) ((a) * (a))
  bool can_read = g_ActiveConfig.bCanReadCameraAngles &&
                  (g_ActiveConfig.bStabilizePitch || g_ActiveConfig.bStabilizeRoll ||
                   g_ActiveConfig.bStabilizeYaw || g_ActiveConfig.bStabilizeX ||
                   g_ActiveConfig.bStabilizeY || g_ActiveConfig.bStabilizeZ);

  if (can_read)
  {
    static int old_vertex_count = 0, old_prim_count = 0;
    // int vertex_count = g_vertex_manager->GetNumberOfVertices();
    // if (vertex_count != old_vertex_count) {
    //	NOTICE_LOG(VR, "*************** vertex_count = %d", vertex_count);
    //	old_vertex_count = vertex_count;
    //}
    int prim_count = g_stats.last_frame.num_prims + g_stats.last_frame.num_dl_prims;
    if (prim_count != old_prim_count)
    {
      //WARN_LOG_FMT(VR, "*************** stats.prevFrame.numPrims = %d       %d", prim_count,
        //       stats.thisFrame.numPrims + stats.thisFrame.numDLPrims);
      old_prim_count = prim_count;
    }
    if (prim_count < (int)g_ActiveConfig.iCameraMinPoly)
      can_read = false;
  }
  if (can_read)
  {
    float* p = constants.posnormalmatrix[0].data();
    Common::Vec3 pos, worldspacepos, movement;
    pos.data[0] = p[0 * 4 + 3];
    pos.data[1] = p[1 * 4 + 3];
    pos.data[2] = p[2 * 4 + 3];
    Common::Matrix33 rot;
    memcpy(&rot.data[0 * 3], &p[0 * 4], 3 * sizeof(float));
    memcpy(&rot.data[1 * 3], &p[1 * 4], 3 * sizeof(float));
    memcpy(&rot.data[2 * 3], &p[2 * 4], 3 * sizeof(float));
    // normalize rotation matrix
    float scale =
        sqrt(sqr(rot.data[0 * 3 + 0]) + sqr(rot.data[0 * 3 + 1]) + sqr(rot.data[0 * 3 + 2]));
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        rot.data[r * 3 + c] /= scale;
    // Position is already in current camera space (has had rot matrix and scale applied to it).
    // Convert camera-space position into world space position, by applying inverse rot matrix.
    // Note that we are not undoing the scale, because game units are measured in camera space after
    // the scale.
    Common::Matrix33 inverse;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        inverse.data[r * 3 + c] = rot.data[c * 3 + r];
    Common::Matrix33::Multiply(inverse, pos, &worldspacepos);
    // Work out how far (in unscaled game units) and in which dimensions it has moved in world space
    // since the previous frame
	static Common::Vec3 oldpos = {0, 0, 0}, totalpos = {0, 0, 0};
    for (int i = 0; i < 3; ++i)
      movement.data[i] = worldspacepos.data[i] - oldpos.data[i];
    float distance = sqrt(sqr(movement.data[0]) + sqr(movement.data[1]) + sqr(movement.data[2]));

    /*INFO_LOG_FMT(VR, "WorldPos: %5.2fm, %5.2fm, %5.2fm; Move: %5.2fm, %5.2fm, %5.2fm; Distance: "
                 "%5.2fm; Scale: x%5.2f",
             pos[0] / g_ActiveConfig.fUnitsPerMetre, pos[1] / g_ActiveConfig.fUnitsPerMetre,
             pos[2] / g_ActiveConfig.fUnitsPerMetre, movement[0] / g_ActiveConfig.fUnitsPerMetre,
             movement[1] / g_ActiveConfig.fUnitsPerMetre,
             movement[2] / g_ActiveConfig.fUnitsPerMetre, distance / g_ActiveConfig.fUnitsPerMetre,
             scale);*/
    // moving more than 2 metres per frame (before VR scaling down to toy size) means we probably
    // jumped to a new object
    // That is 216 kilometres per hour (135 miles per hour) at 30 FPS, or 432 kph (270 mph) at 60
    // FPS
    // so only add actual camera motion, not jumps, to totalpos
    if (distance / g_ActiveConfig.fUnitsPerMetre <= 2.0f &&
        (oldpos.data[0] != 0 || oldpos.data[1] != 0 || oldpos.data[2] != 0))
    {
      for (int i = 0; i < 3; ++i)
        totalpos.data[i] += movement.data[i];
      //INFO_LOG_FMT(VR, "Total Pos: %5.2f, %5.2f, %5.2f", totalpos[0], totalpos[1], totalpos[2]);
    }
    for (int i = 0; i < 3; ++i)
      oldpos.data[i] = worldspacepos.data[i];
    // rotate total position back into current game-camera space, and save it globally in metres
    Common::Matrix33::Multiply(rot, totalpos, &g_game_camera_pos);
    for (int i = 0; i < 3; ++i)
      g_game_camera_pos.data[i] = g_game_camera_pos.data[i] / g_ActiveConfig.fUnitsPerMetre;
    //INFO_LOG_FMT(VR, "g_game_camera_pos: %5.2fm, %5.2fm, %5.2fm", g_game_camera_pos[0],
    //         g_game_camera_pos[1], g_game_camera_pos[2]);

    // add pitch to rotation matrix
    if (g_ActiveConfig.fReadPitch != 0)
    {
      Common::Matrix33 rp, result;
      rp *= Common::Matrix33::RotateX(DEGREES_TO_RADIANS(g_ActiveConfig.fReadPitch));
      Common::Matrix33::Multiply(rot, rp, &result);
      memcpy(rot.data.data(), result.data.data(), 3 * 3 * sizeof(float));
    }
    // extract yaw, pitch, and roll from rotation matrix
    float yaw, pitch, roll;
    Common::Matrix33::GetPieYawPitchRollR(rot, yaw, pitch, roll);

    if (abs(roll) == 3.1415926535f)
    {
      roll = 0;  // Unlikely the camera should actually be flipped exactly 180 degrees. We most
                 // likely chose the wrong object.
    }

    // if (abs(yaw) == 3.1415926535f)
    //{
    //	yaw = 0; // Unlikely the camera should actually be flipped exactly 180 degrees. We most
    // likely chose the wrong object.
    //}

    if (g_ActiveConfig.bKeyhole)
    {
      static float keyhole_center = 0;
      float keyhole_snap = 0;

      if (g_ActiveConfig.bKeyholeSnap)
        keyhole_snap = DEGREES_TO_RADIANS(g_ActiveConfig.fKeyholeSnapSize);

      float keyhole_width = DEGREES_TO_RADIANS(g_ActiveConfig.fKeyholeWidth / 2);
      float keyhole_left_bound = keyhole_center + keyhole_width;
      float keyhole_right_bound = keyhole_center - keyhole_width;

      // Correct left and right bounds if they calculated incorrectly and are out of the range of
      // -PI to PI.
      if (keyhole_left_bound > (float)(M_PI))
        keyhole_left_bound -= (2 * (float)(M_PI));
      else if (keyhole_right_bound < -(float)(M_PI))
        keyhole_right_bound += (2 * (float)(M_PI));

      // Crossing from positive to negative half, counter-clockwise
      if (yaw < 0 && keyhole_left_bound > 0 && keyhole_right_bound > 0 &&
          yaw < keyhole_width - (float)(M_PI))
      {
        keyhole_center = yaw - keyhole_width + keyhole_snap;
      }
      // Crossing from negative to positive half, clockwise
      else if (yaw > 0 && keyhole_left_bound < 0 && keyhole_right_bound < 0 &&
               yaw > (float)(M_PI)-keyhole_width)
      {
        keyhole_center = yaw + keyhole_width - keyhole_snap;
      }
      // Already within the negative and positive range
      else if (keyhole_left_bound < 0 && keyhole_right_bound > 0)
      {
        if (yaw < keyhole_right_bound && yaw > 0)
          keyhole_center = yaw + keyhole_width - keyhole_snap;
        else if (yaw > keyhole_left_bound && yaw < 0)
          keyhole_center = yaw - keyhole_width + keyhole_snap;
      }
      // Anywhere within the normal range
      else
      {
        if (yaw < keyhole_right_bound)
          keyhole_center = yaw + keyhole_width - keyhole_snap;
        else if (yaw > keyhole_left_bound)
          keyhole_center = yaw - keyhole_width + keyhole_snap;
      }

      yaw -= keyhole_center;
    }

    // NOTICE_LOG(VR, "Pos(%d): %5.2f, %5.2f, %5.2f; scale: x%5.2f",
    // g_main_cp_state.matrix_index_a.PosNormalMtxIdx, pos[0], pos[1], pos[2], scale);
    // debug - show which object is being used
    // static float first_x = 0;
    // if (first_x == 0)
    //	first_x = pos[0];
    // else if (g_ActiveConfig.iFlashState > 5)
    //	constants.posnormalmatrix[0][0 * 4 + 3] = first_x;

    Common::Matrix33 matrix_pitch, matrix_yaw, matrix_roll, temp;

    if (g_ActiveConfig.bStabilizeRoll && g_ActiveConfig.bStabilizeYaw)
    {
      matrix_roll *= Common::Matrix33::RotateZ(-roll);
      matrix_yaw *= Common::Matrix33::RotateY(yaw);
      Common::Matrix33::Multiply(matrix_roll, matrix_yaw, &temp);
    }
    else if (g_ActiveConfig.bStabilizeRoll)
    {
      matrix_roll *= Common::Matrix33::RotateZ(-roll);
      temp = matrix_roll;
    }
    else if (g_ActiveConfig.bStabilizeYaw)
    {
      matrix_yaw *= Common::Matrix33::RotateY(yaw);
      temp = matrix_yaw;
    }
    else
    {
      temp = Common::Matrix33::Identity();
    }

    if (g_ActiveConfig.bStabilizePitch)
    {
      matrix_pitch *= Common::Matrix33::RotateX(-pitch);
      rot = matrix_pitch * temp;
      g_game_camera_rotmat = Common::Matrix44::FromMatrix33(rot);
    }
    else
    {
      g_game_camera_rotmat = Common::Matrix44::FromMatrix33(temp);
    }
  }
  else
  {
    g_game_camera_rotmat = Common::Matrix44::Identity();
    memset(g_game_camera_pos.data.data(), 0, 3 * sizeof(float));
  }
}

void VertexShaderManager::CheckSkybox()
{
  if (xfmem.projection.type == ProjectionType::Perspective)
  {
    float* p = constants.posnormalmatrix[0].data();
    float pos[3];
    pos[0] = p[0 * 4 + 3];
    pos[1] = p[1 * 4 + 3];
    pos[2] = p[2 * 4 + 3];
    // If we are drawing at precisely the origin (camera position) it's probably a skybox
    if (pos[0] == 0 && pos[1] == 0 && pos[2] == 0)
    {
      if (p[0 * 4 + 0] != 1.0f)
      {
        // ERROR_LOG(VR, "SKYBOX!!!!");
        g_is_skybox = true;
      }
      else
      {
        // ERROR_LOG(VR, "NOT a skybox! Identity matrix.");
      }
    }
  }
}

void VertexShaderManager::LockSkybox()
{
  static float s_locked_skybox[3 * 4];
  static bool s_had_skybox = false;

  if (xfmem.projection.type == ProjectionType::Perspective)
  {
    float* p = constants.posnormalmatrix[0].data();
    if (s_had_skybox)
    {
      memcpy(p, s_locked_skybox, 4 * 3 * sizeof(float));
    }
    else
    {
      memcpy(s_locked_skybox, p, 4 * 3 * sizeof(float));
      s_had_skybox = true;
    }
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
