// Copyright 2016 Dolphin VR Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/VRGameMatrices.h"
#include "Common/Common.h"
#include "Common/FileUtil.h"
#include "Common/MathUtil.h"
#include "Common/MsgHandler.h"
#include "VideoBackends/D3D/AvatarDrawer.h"
#include "VideoBackends/D3D/D3DBase.h"
//#include "VideoBackends/D3D/D3DBlob.h"
//#include "VideoBackends/D3D/D3DShader.h"
#include "VideoBackends/D3D/D3DState.h"
//#include "VideoBackends/D3D/FramebufferManager.h"
//#include "VideoBackends/D3D/GeometryShaderCache.h"
//#include "VideoBackends/D3D/Render.h"
#include "VideoBackends/D3D/tiny_obj_loader.h"
#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/VR.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

//#include "InputCommon/ControllerInterface/Sixense/SixenseHack.h"

extern float s_fViewTranslationVector[3];
// extern EFBRectangle g_final_screen_region; // Replaced with GetFinalScreenRegion()

#include "VideoCommon/Present.h" // For GetFinalScreenRegion()

bool CalculateViewMatrix(int kind, Common::Matrix44& look_matrix)
{
  bool bStuckToHead = false, bIsSkybox = false, bIsPerspective = false,
       bHasWidest = (vr_widest_3d_HFOV > 0);
  bool bIsHudElement = false, bIsOffscreen = false, bAspectWide = true, bNoForward = false,
       bShowAim = false;
  float UnitsPerMetre = 1.0f;

  // Show HUD
  if (kind == 1)
  {
    if (!bHasWidest)
      return false;
  }
  // Show Aim
  else if (kind == 0)
  {
    if (bHasWidest)
      bShowAim = true;
  }
  // Show 2D
  else if (kind == 2)
  {
    bHasWidest = false;
  }

  float zoom_forward = 0.0f;
  if (vr_widest_3d_HFOV <= g_ActiveConfig.fMinFOV && bHasWidest)
  {
    zoom_forward = g_ActiveConfig.fAimDistance *
                   tanf(((g_ActiveConfig.fMinFOV) * Common::PI / 180.0f) / 2) /
                   tanf(((vr_widest_3d_HFOV) * Common::PI / 180.0f) / 2);
    zoom_forward -= g_ActiveConfig.fAimDistance;
  }

  float hfov = 0, vfov = 0;
  hfov = vr_widest_3d_HFOV;
  vfov = vr_widest_3d_VFOV;

  // VR Headtracking and leaning back compensation
  Common::Matrix44 rotation_matrix;
  Common::Matrix44 lean_back_matrix;
  Common::Matrix44 camera_pitch_matrix;
  if (bStuckToHead)
  {
    rotation_matrix = Common::Matrix44::Identity();
    lean_back_matrix = Common::Matrix44::Identity();
    camera_pitch_matrix = Common::Matrix44::Identity();
  }
  else
  {
    // head tracking
    if (g_ActiveConfig.bOrientationTracking)
    {
      VR_UpdateHeadTrackingIfNeeded();
      rotation_matrix = g_head_tracking_matrix;
    }
    else
    {
      rotation_matrix = Common::Matrix44::Identity();
    }

    Common::Matrix33 pitch_matrix33;

    // leaning back
    float extra_pitch = -g_ActiveConfig.fLeanBackAngle;
    pitch_matrix33 = Common::Matrix33::RotateX(-((extra_pitch) * Common::PI / 180.0f));
    lean_back_matrix = Common::Matrix44::FromMatrix33(pitch_matrix33);

    // camera pitch
    if ((g_ActiveConfig.bStabilizePitch || g_ActiveConfig.bStabilizeRoll ||
         g_ActiveConfig.bStabilizeYaw) &&
        g_ActiveConfig.bCanReadCameraAngles &&
        (g_ActiveConfig.iMotionSicknessSkybox != 2 || !bIsSkybox))
    {
      if (!g_ActiveConfig.bStabilizePitch)
      {
        Common::Matrix33 user_pitch_m33; // Changed variable name to avoid conflict and denote type
        Common::Matrix44 roll_and_yaw_matrix;

        if (bIsPerspective || bHasWidest)
          extra_pitch = g_ActiveConfig.fCameraPitch;
        else
          extra_pitch = g_ActiveConfig.fScreenPitch;
        user_pitch_m33 = Common::Matrix33::RotateX(-((extra_pitch) * Common::PI / 180.0f));
        // user_pitch44 = pitch_matrix33; // Original line, user_pitch44 was Matrix44
        roll_and_yaw_matrix = g_game_camera_rotmat; // This is Matrix44
        camera_pitch_matrix = Common::Matrix44::FromMatrix33(user_pitch_m33) * roll_and_yaw_matrix;
      }
      else
      {
        camera_pitch_matrix = g_game_camera_rotmat;
      }
    }
    else
    {
      if (xfmem.projection.type == ProjectionType::Perspective || bHasWidest)
        extra_pitch = g_ActiveConfig.fCameraPitch;
      else
        extra_pitch = g_ActiveConfig.fScreenPitch;
      pitch_matrix33 = Common::Matrix33::RotateX(-((extra_pitch) * Common::PI / 180.0f));
      camera_pitch_matrix = Common::Matrix44::FromMatrix33(pitch_matrix33);
    }
  }

  // Position matrices
  Common::Matrix44 head_position_matrix, free_look_matrix, camera_forward_matrix, camera_position_matrix;
  if (bStuckToHead || bIsSkybox)
  {
    head_position_matrix = Common::Matrix44::Identity();
    free_look_matrix = Common::Matrix44::Identity();
    camera_position_matrix = Common::Matrix44::Identity();
  }
  else
  {
    float pos[3];
    // head tracking
    if (g_ActiveConfig.bPositionTracking)
    {
      for (int i = 0; i < 3; ++i)
        pos[i] = g_head_tracking_position[i] * UnitsPerMetre;
      head_position_matrix = Common::Matrix44::Translate(Common::Vec3(pos[0], pos[1], pos[2]));
    }
    else
    {
      head_position_matrix = Common::Matrix44::Identity();
    }

    // freelook camera position
    for (int i = 0; i < 3; ++i)
      pos[i] = s_fViewTranslationVector[i] * UnitsPerMetre;
    free_look_matrix = Common::Matrix44::Translate(Common::Vec3(pos[0], pos[1], pos[2]));

    // camera position stabilisation
    if (g_ActiveConfig.bStabilizeX || g_ActiveConfig.bStabilizeY || g_ActiveConfig.bStabilizeZ)
    {
      for (int i = 0; i < 3; ++i)
        pos[i] = -g_game_camera_pos[i] * UnitsPerMetre;
      camera_position_matrix = Common::Matrix44::Translate(Common::Vec3(pos[0], pos[1], pos[2]));
    }
    else
    {
      camera_position_matrix = Common::Matrix44::Identity();
    }
  }

  if (bIsPerspective && !bIsHudElement && !bIsOffscreen)
  {
    // Transformations must be applied in the following order for VR:
    // camera position stabilisation
    // camera forward
    // camera pitch
    // free look
    // leaning back
    // head position tracking
    // head rotation tracking
    if (bNoForward || bIsSkybox || bStuckToHead)
    {
      camera_forward_matrix = Common::Matrix44::Identity();
    }
    else
    {
      float pos[3];
      pos[0] = 0;
      pos[1] = 0;
      pos[2] = (g_ActiveConfig.fCameraForward + zoom_forward) * UnitsPerMetre;
      camera_forward_matrix = Common::Matrix44::Translate(Common::Vec3(pos[0], pos[1], pos[2]));
    }

    look_matrix = camera_forward_matrix * camera_position_matrix * camera_pitch_matrix *
                  free_look_matrix * lean_back_matrix * head_position_matrix * rotation_matrix;
  }
  else
  {
    float HudWidth, HudHeight, HudThickness, HudDistance, HudUp, CameraForward, AimDistance;

    // 2D Screen
    if (!bHasWidest)
    {
      HudThickness = g_ActiveConfig.fScreenThickness * UnitsPerMetre;
      HudDistance = g_ActiveConfig.fScreenDistance * UnitsPerMetre;
      HudHeight = g_ActiveConfig.fScreenHeight * UnitsPerMetre;
      HudHeight = g_ActiveConfig.fScreenHeight * UnitsPerMetre;
      // NES games are supposed to be 1.175:1 (16:13.62) even though VC normally renders them as
      // 16:9
      // http://forums.nesdev.com/viewtopic.php?t=8063
      if (g_is_nes)
        HudWidth = HudHeight * 1.175f;
      else if (bAspectWide)
        HudWidth = HudHeight * (float)16 / 9;
      else
        HudWidth = HudHeight * (float)4 / 3;
      CameraForward = 0;
      HudUp = g_ActiveConfig.fScreenUp * UnitsPerMetre;
      AimDistance = HudDistance;
    }
    else
    // HUD over 3D world
    {
      // The HUD distance might have been carefully chosen to line up with objects, so we should
      // scale it with the world
      // But we can't make the HUD too close or it's hard to look at, and we should't make the HUD
      // too far or it stops looking 3D
      const float MinHudDistance = 0.28f, MaxHudDistance = 3.00f;  // HUD shouldn't go closer than
                                                                   // 28 cm when shrinking scale, or
                                                                   // further than 3m when growing
      float HUDScale = g_ActiveConfig.fScale;
      if (HUDScale < 1.0f && g_ActiveConfig.fHudDistance >= MinHudDistance &&
          g_ActiveConfig.fHudDistance * HUDScale < MinHudDistance)
        HUDScale = MinHudDistance / g_ActiveConfig.fHudDistance;
      else if (HUDScale > 1.0f && g_ActiveConfig.fHudDistance <= MaxHudDistance &&
               g_ActiveConfig.fHudDistance * HUDScale > MaxHudDistance)
        HUDScale = MaxHudDistance / g_ActiveConfig.fHudDistance;

      // Give the 2D layer a 3D effect if different parts of the 2D layer are rendered at different
      // z coordinates
      HudThickness = g_ActiveConfig.fHudThickness * HUDScale *
                     UnitsPerMetre;  // the 2D layer is actually a 3D box this many game units thick
      HudDistance = g_ActiveConfig.fHudDistance * HUDScale *
                    UnitsPerMetre;  // depth 0 on the HUD should be this far away

      HudUp = 0;
      if (bNoForward)
        CameraForward = 0;
      else
        CameraForward =
            (g_ActiveConfig.fCameraForward + zoom_forward) * g_ActiveConfig.fScale * UnitsPerMetre;
      // When moving the camera forward, correct the size of the HUD so that aiming is correct at
      // AimDistance
      AimDistance = g_ActiveConfig.fAimDistance * g_ActiveConfig.fScale * UnitsPerMetre;
      if (AimDistance <= 0)
        AimDistance = HudDistance;
      if (bShowAim)
      {
        HudThickness = 0;
        HudDistance = AimDistance;
        HUDScale = g_ActiveConfig.fScale;
      }
      // Now that we know how far away the box is, and what FOV it should fill, we can work out the
      // width and height in game units
      // Note: the HUD won't line up exactly (except at AimDistance) if CameraForward is non-zero
      // float HudWidth = 2.0f * tanf(hfov / 2.0f * 3.14159f / 180.0f) * (HudDistance) * Correction;
      // float HudHeight = 2.0f * tanf(vfov / 2.0f * 3.14159f / 180.0f) * (HudDistance) *
      // Correction;
      HudWidth = 2.0f * tanf(((hfov / 2.0f) * Common::PI / 180.0f)) * HudDistance *
                 (AimDistance + CameraForward) / AimDistance;
      HudHeight = 2.0f * tanf(((vfov / 2.0f) * Common::PI / 180.0f)) * HudDistance *
                  (AimDistance + CameraForward) / AimDistance;
    }

    float scale[3];  // width, height, and depth of box in game units divided by 2D width, height,
                     // and depth
    float position[3];  // position of front of box relative to the camera, in game units

    float viewport_scale[2];
    float viewport_offset[2];  // offset as a fraction of the viewport's width
    if (!bIsHudElement && !bIsOffscreen)
    {
      viewport_scale[0] = 1.0f;
      viewport_scale[1] = 1.0f;
      viewport_offset[0] = 0.0f;
      viewport_offset[1] = 0.0f;
    }
    else
    {
      Viewport& v = xfmem.viewport;
      float left, top, width, height;
      left = v.xOrig - v.wd - 342;
      top = v.yOrig + v.ht - 342;
      width = 2 * v.wd;
      height = -2 * v.ht;
      float screen_width = (float)GetFinalScreenRegion().GetWidth();
      float screen_height = (float)GetFinalScreenRegion().GetHeight();
      viewport_scale[0] = width / screen_width;
      viewport_scale[1] = height / screen_height;
      viewport_offset[0] = ((left + (width / 2)) - (0 + (screen_width / 2))) / screen_width;
      viewport_offset[1] = -((top + (height / 2)) - (0 + (screen_height / 2))) / screen_height;
    }

    // 3D HUD elements (may be part of 2D screen or HUD)
    if (bIsPerspective)
    {
      // these are the edges of the near clipping plane in game coordinates
      float left2D = 0;
      float right2D = 1;
      float bottom2D = 1;
      float top2D = 0;
      float zFar2D = 1;
      float zNear2D = -1;
      float zObj = zNear2D + (zFar2D - zNear2D) * g_ActiveConfig.fHud3DCloser;

      left2D *= zObj;
      right2D *= zObj;
      bottom2D *= zObj;
      top2D *= zObj;

      // Scale the width and height to fit the HUD in metres
      if (right2D == left2D)
      {
        scale[0] = 0;
      }
      else
      {
        scale[0] = viewport_scale[0] * HudWidth / (right2D - left2D);
      }
      if (top2D == bottom2D)
      {
        scale[1] = 0;
      }
      else
      {
        scale[1] = viewport_scale[1] * HudHeight /
                   (top2D - bottom2D);  // note that positive means up in 3D
      }
      // Keep depth the same scale as width, so it looks like a real object
      if (zFar2D == zNear2D)
      {
        scale[2] = scale[0];
      }
      else
      {
        scale[2] = scale[0];
      }
      // Adjust the position for off-axis projection matrices, and shifting the 2D screen
      position[0] = scale[0] * (-(right2D + left2D) / 2.0f) +
                    viewport_offset[0] * HudWidth;  // shift it right into the centre of the view
      position[1] = scale[1] * (-(top2D + bottom2D) / 2.0f) + viewport_offset[1] * HudHeight +
                    HudUp;  // shift it up into the centre of the view;
      // Shift it from the old near clipping plane to the HUD distance, and shift the camera forward
      if (!bHasWidest)
        position[2] = scale[2] * zObj - HudDistance;
      else
        position[2] = scale[2] * zObj - HudDistance;  // - CameraForward;
    }
    // 2D layer, or 2D viewport (may be part of 2D screen or HUD)
    else
    {
      float left2D = 0;
      float right2D = 1;
      float bottom2D = 1;
      float top2D = 0;
      float zNear2D = -1;
      float zFar2D = 1;

      // for 2D, work out the fraction of the HUD we should fill, and multiply the scale by that
      // also work out what fraction of the height we should shift it up, and what fraction of the
      // width we should shift it left
      // only multiply by the extra scale after adjusting the position?

      if (right2D == left2D)
      {
        scale[0] = 0;
      }
      else
      {
        scale[0] = viewport_scale[0] * HudWidth / (right2D - left2D);
      }
      if (top2D == bottom2D)
      {
        scale[1] = 0;
      }
      else
      {
        scale[1] = viewport_scale[1] * HudHeight /
                   (top2D - bottom2D);  // note that positive means up in 3D
      }
      if (zFar2D == zNear2D)
      {
        scale[2] = 0;  // The 2D layer was flat, so we make it flat instead of a box to avoid
                       // dividing by zero
      }
      else
      {
        scale[2] = HudThickness /
                   (zFar2D -
                    zNear2D);  // Scale 2D z values into 3D game units so it is the right thickness
      }
      position[0] = scale[0] * (-(right2D + left2D) / 2.0f) +
                    viewport_offset[0] * HudWidth;  // shift it right into the centre of the view
      position[1] = scale[1] * (-(top2D + bottom2D) / 2.0f) + viewport_offset[1] * HudHeight +
                    HudUp;  // shift it up into the centre of the view;
      // Shift it from the zero plane to the HUD distance, and shift the camera forward
      if (!bHasWidest)
        position[2] = -HudDistance;
      else
        position[2] = -HudDistance;  // - CameraForward;
    }

    Common::Matrix44 scale_matrix, position_matrix;
    // Common::Matrix44::Scale(scale_matrix, scale);
    {
      Common::Matrix33 m33_scale = Common::Matrix33::Scale(Common::Vec3(scale[0], scale[1], scale[2]));
      scale_matrix = Common::Matrix44::FromMatrix33(m33_scale);
    }
    position_matrix = Common::Matrix44::Translate(Common::Vec3(position[0], position[1], position[2]));

    // order: scale, position
    look_matrix = scale_matrix * position_matrix * camera_position_matrix * camera_pitch_matrix *
                  free_look_matrix * lean_back_matrix * head_position_matrix * rotation_matrix;
  }
  return true;
}

void VRCalculateIRPointer()
{
  float wmpos[3], thumb[3];
  Common::Matrix33 wmrot;
  ControllerStyle cs = CS_HYDRA_RIGHT;
  bool has_right_controller = VR_GetRightControllerPos(wmpos, thumb, &wmrot);
  if (has_right_controller)
    cs = VR_GetHydraStyle(1);
  if (cs != CS_WIIMOTE_IR)
  {
    g_vr_has_ir = false;
    return;
  }

  Common::Matrix44 ToAimSpace, WiimoteRot;
  CalculateTrackingSpaceToViewSpaceMatrix(0, ToAimSpace);
  WiimoteRot = Common::Matrix44::FromMatrix33(wmrot);

  float r[3], rp[3], d[3], v_arr[3] = {0, 0, -1.0f}, ppos[3], aimpoint[3]; // Renamed v to v_arr to avoid conflict with Vec4

  // find pointer position
  // Common::Matrix44::Multiply(WiimoteRot, v, ppos);
  Common::Vec4 v_vec4(v_arr[0], v_arr[1], v_arr[2], 0.0f); // Direction vector, w=0
  Common::Vec4 ppos_vec4 = WiimoteRot * v_vec4;
  ppos[0] = ppos_vec4.x;
  ppos[1] = ppos_vec4.y;
  ppos[2] = ppos_vec4.z;

  for (int i = 0; i < 3; ++i)
    ppos[i] += wmpos[i]; // wmpos is also float[3], ppos is now populated

  // Common::Matrix44::Multiply(ToAimSpace, wmpos, r);
  Common::Vec4 wmpos_vec4(wmpos[0], wmpos[1], wmpos[2], 1.0f); // Position vector, w=1
  Common::Vec4 r_vec4 = ToAimSpace * wmpos_vec4;
  r[0] = r_vec4.x;
  r[1] = r_vec4.y;
  r[2] = r_vec4.z;

  // Common::Matrix44::Multiply(ToAimSpace, ppos, rp);
  Common::Vec4 ppos_intermediate_vec4(ppos[0], ppos[1], ppos[2], 1.0f); // Position vector, w=1
  Common::Vec4 rp_vec4 = ToAimSpace * ppos_intermediate_vec4;
  rp[0] = rp_vec4.x;
  rp[1] = rp_vec4.y;
  rp[2] = rp_vec4.z;

  for (int i = 0; i < 3; ++i)
    d[i] = rp[i] - r[i];
  float s = -r[2] / d[2];
  if (s < 0)
  {
    // aimed away from screen, so set aim point to infinity on the closest side
    // this is for first person games where we will turn the camera towards the IR point if it is
    // off-screen
    s = std::numeric_limits<float>::infinity();
  }

  for (int i = 0; i < 3; ++i)
    aimpoint[i] = r[i] + d[i] * s;

  // NOTICE_LOG(VR, "r=%8f, %8f, %8f       %8f     d=%8f, %8f, %8f     a=%8f, %8f, %8f", r[0], r[1],
  // r[2], rp[2]-r[2], d[0], d[1], d[2], aimpoint[0], aimpoint[1], aimpoint[2]);
  g_vr_ir_x = aimpoint[0] * 2 - 1;
  g_vr_ir_y = 1 - (aimpoint[1] * 2);
  g_vr_ir_z = aimpoint[2];  // todo: currently always 0, but should be based on actual controller
                            // distance from screen, not aimpoint
  g_vr_has_ir = true;
}

bool CalculateTrackingSpaceToViewSpaceMatrix(int kind, Common::Matrix44& look_matrix)
{
  auto invert_rotation_part = [](Common::Matrix44& mat) {
    Common::Matrix33 rot_part;
    // Extract 3x3 part
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        rot_part.data[r * 3 + c] = mat.data[r * 4 + c];
      }
    }
    // Extract translation part
    Common::Vec3 trans_part = {mat.data[3], mat.data[7], mat.data[11]};

    rot_part = rot_part.Inverted(); // Invert the 3x3 part

    // Create new matrix from inverted 3x3 part
    mat = Common::Matrix44::FromMatrix33(rot_part);
    // Re-apply original translation
    mat.data[3] = trans_part.x;
    mat.data[7] = trans_part.y;
    mat.data[11] = trans_part.z;
  };

  bool bStuckToHead = false, bIsSkybox = false, bIsPerspective = false,
       bHasWidest = (vr_widest_3d_HFOV > 0);
  bool bIsHudElement = false, bIsOffscreen = false, bAspectWide = true, bNoForward = false,
       bShowAim = false;
  float UnitsPerMetre = 1.0f;

  // HUD space
  if (kind == 1)
  {
    if (!bHasWidest)
      return false;
  }
  // Aim space
  else if (kind == 0)
  {
    if (bHasWidest)
      bShowAim = true;
  }
  // 2D space
  else if (kind == 2)
  {
    bHasWidest = false;
  }

  float zoom_forward = 0.0f;
  if (vr_widest_3d_HFOV <= g_ActiveConfig.fMinFOV && bHasWidest)
  {
    zoom_forward = g_ActiveConfig.fAimDistance *
                   tanf(((g_ActiveConfig.fMinFOV) * Common::PI / 180.0f) / 2) /
                   tanf(((vr_widest_3d_HFOV) * Common::PI / 180.0f) / 2);
    zoom_forward -= g_ActiveConfig.fAimDistance;
  }

  float hfov = 0, vfov = 0;
  hfov = vr_widest_3d_HFOV;
  vfov = vr_widest_3d_VFOV;

  // VR ~Headtracking and~ leaning back compensation
  Common::Matrix44 rotation_matrix;
  Common::Matrix44 lean_back_matrix;
  Common::Matrix44 camera_pitch_matrix;
  if (bStuckToHead)
  {
    rotation_matrix = Common::Matrix44::Identity();
    lean_back_matrix = Common::Matrix44::Identity();
    camera_pitch_matrix = Common::Matrix44::Identity();
  }
  else
  {
    // no head tracking
    rotation_matrix = Common::Matrix44::Identity();

    Common::Matrix33 pitch_matrix33;

    // leaning back
    float extra_pitch = -g_ActiveConfig.fLeanBackAngle;
    pitch_matrix33 = Common::Matrix33::RotateX(-((extra_pitch) * Common::PI / 180.0f));
    lean_back_matrix = Common::Matrix44::FromMatrix33(pitch_matrix33);

    // camera pitch
    if ((g_ActiveConfig.bStabilizePitch || g_ActiveConfig.bStabilizeRoll ||
         g_ActiveConfig.bStabilizeYaw) &&
        g_ActiveConfig.bCanReadCameraAngles &&
        (g_ActiveConfig.iMotionSicknessSkybox != 2 || !bIsSkybox))
    {
      if (!g_ActiveConfig.bStabilizePitch)
      {
        Common::Matrix33 user_pitch_m33; // Changed variable name to avoid conflict and denote type
        Common::Matrix44 roll_and_yaw_matrix;

        if (bIsPerspective || bHasWidest)
          extra_pitch = g_ActiveConfig.fCameraPitch;
        else
          extra_pitch = g_ActiveConfig.fScreenPitch;
        user_pitch_m33 = Common::Matrix33::RotateX(-((extra_pitch) * Common::PI / 180.0f));
        // user_pitch44 = pitch_matrix33; // Original line, user_pitch44 was Matrix44
        roll_and_yaw_matrix = g_game_camera_rotmat; // This is Matrix44
        camera_pitch_matrix = Common::Matrix44::FromMatrix33(user_pitch_m33) * roll_and_yaw_matrix;
      }
      else
      {
        camera_pitch_matrix = g_game_camera_rotmat;
      }
    }
    else
    {
      if (xfmem.projection.type == ProjectionType::Perspective || bHasWidest)
        extra_pitch = g_ActiveConfig.fCameraPitch;
      else
        extra_pitch = g_ActiveConfig.fScreenPitch;
      pitch_matrix33 = Common::Matrix33::RotateX(-((extra_pitch) * Common::PI / 180.0f));
      camera_pitch_matrix = Common::Matrix44::FromMatrix33(pitch_matrix33);
    }
  }

  // Position matrices
  Common::Matrix44 head_position_matrix, free_look_matrix, camera_forward_matrix, camera_position_matrix;
  if (bStuckToHead || bIsSkybox)
  {
    head_position_matrix = Common::Matrix44::Identity();
    free_look_matrix = Common::Matrix44::Identity();
    camera_position_matrix = Common::Matrix44::Identity();
  }
  else
  {
    float pos[3];
    // no head tracking
    head_position_matrix = Common::Matrix44::Identity();

    // freelook camera position
    for (int i = 0; i < 3; ++i)
      pos[i] = s_fViewTranslationVector[i] * UnitsPerMetre;
    free_look_matrix = Common::Matrix44::Translate(Common::Vec3(pos[0], pos[1], pos[2]));

    // camera position stabilisation
    if (g_ActiveConfig.bStabilizeX || g_ActiveConfig.bStabilizeY || g_ActiveConfig.bStabilizeZ)
    {
      for (int i = 0; i < 3; ++i)
        pos[i] = -g_game_camera_pos[i] * UnitsPerMetre;
      camera_position_matrix = Common::Matrix44::Translate(Common::Vec3(pos[0], pos[1], pos[2]));
    }
    else
    {
      camera_position_matrix = Common::Matrix44::Identity();
    }
  }

  if (bIsPerspective && !bIsHudElement && !bIsOffscreen)
  {
    // Transformations must be applied in the following order for VR:
    // camera position stabilisation
    // camera forward
    // camera pitch
    // free look
    // leaning back
    // head position tracking
    // head rotation tracking
    if (bNoForward || bIsSkybox || bStuckToHead)
    {
      camera_forward_matrix = Common::Matrix44::Identity();
    }
    else
    {
      float pos[3];
      pos[0] = 0;
      pos[1] = 0;
      pos[2] = (g_ActiveConfig.fCameraForward + zoom_forward) * UnitsPerMetre;
      camera_forward_matrix = Common::Matrix44::Translate(Common::Vec3(pos[0], pos[1], pos[2]));
    }

    invert_rotation_part(camera_pitch_matrix);
    invert_rotation_part(lean_back_matrix);
    invert_rotation_part(rotation_matrix);
    camera_position_matrix = Common::Matrix44::Translate(Common::Vec3(-camera_position_matrix.data[3], -camera_position_matrix.data[7], -camera_position_matrix.data[11]));
    camera_forward_matrix = Common::Matrix44::Translate(Common::Vec3(-camera_forward_matrix.data[3], -camera_forward_matrix.data[7], -camera_forward_matrix.data[11]));
    free_look_matrix = Common::Matrix44::Translate(Common::Vec3(-free_look_matrix.data[3], -free_look_matrix.data[7], -free_look_matrix.data[11]));
    head_position_matrix = Common::Matrix44::Translate(Common::Vec3(-head_position_matrix.data[3], -head_position_matrix.data[7], -head_position_matrix.data[11]));

    look_matrix = rotation_matrix * head_position_matrix * lean_back_matrix * free_look_matrix *
                  camera_pitch_matrix * camera_position_matrix * camera_forward_matrix;
  }
  else
  {
    float HudWidth, HudHeight, HudThickness, HudDistance, HudUp, CameraForward, AimDistance;

    // 2D Screen
    if (!bHasWidest)
    {
      HudThickness = g_ActiveConfig.fScreenThickness * UnitsPerMetre;
      HudDistance = g_ActiveConfig.fScreenDistance * UnitsPerMetre;
      HudHeight = g_ActiveConfig.fScreenHeight * UnitsPerMetre;
      HudHeight = g_ActiveConfig.fScreenHeight * UnitsPerMetre;
      // NES games are supposed to be 1.175:1 (16:13.62) even though VC normally renders them as
      // 16:9
      // http://forums.nesdev.com/viewtopic.php?t=8063
      if (g_is_nes)
        HudWidth = HudHeight * 1.175f;
      else if (bAspectWide)
        HudWidth = HudHeight * (float)16 / 9;
      else
        HudWidth = HudHeight * (float)4 / 3;
      CameraForward = 0;
      HudUp = g_ActiveConfig.fScreenUp * UnitsPerMetre;
      AimDistance = HudDistance;
    }
    else
    // HUD over 3D world
    {
      // The HUD distance might have been carefully chosen to line up with objects, so we should
      // scale it with the world
      // But we can't make the HUD too close or it's hard to look at, and we should't make the HUD
      // too far or it stops looking 3D
      const float MinHudDistance = 0.28f, MaxHudDistance = 3.00f;  // HUD shouldn't go closer than
                                                                   // 28 cm when shrinking scale, or
                                                                   // further than 3m when growing
      float HUDScale = g_ActiveConfig.fScale;
      if (HUDScale < 1.0f && g_ActiveConfig.fHudDistance >= MinHudDistance &&
          g_ActiveConfig.fHudDistance * HUDScale < MinHudDistance)
        HUDScale = MinHudDistance / g_ActiveConfig.fHudDistance;
      else if (HUDScale > 1.0f && g_ActiveConfig.fHudDistance <= MaxHudDistance &&
               g_ActiveConfig.fHudDistance * HUDScale > MaxHudDistance)
        HUDScale = MaxHudDistance / g_ActiveConfig.fHudDistance;

      // Give the 2D layer a 3D effect if different parts of the 2D layer are rendered at different
      // z coordinates
      HudThickness = g_ActiveConfig.fHudThickness * HUDScale *
                     UnitsPerMetre;  // the 2D layer is actually a 3D box this many game units thick
      HudDistance = g_ActiveConfig.fHudDistance * HUDScale *
                    UnitsPerMetre;  // depth 0 on the HUD should be this far away

      HudUp = 0;
      if (bNoForward)
        CameraForward = 0;
      else
        CameraForward =
            (g_ActiveConfig.fCameraForward + zoom_forward) * g_ActiveConfig.fScale * UnitsPerMetre;
      // When moving the camera forward, correct the size of the HUD so that aiming is correct at
      // AimDistance
      AimDistance = g_ActiveConfig.fAimDistance * g_ActiveConfig.fScale * UnitsPerMetre;
      if (AimDistance <= 0)
        AimDistance = HudDistance;
      if (bShowAim)
      {
        HudThickness = 0;
        HudDistance = AimDistance;
        HUDScale = g_ActiveConfig.fScale;
      }
      // Now that we know how far away the box is, and what FOV it should fill, we can work out the
      // width and height in game units
      // Note: the HUD won't line up exactly (except at AimDistance) if CameraForward is non-zero
      // float HudWidth = 2.0f * tanf(hfov / 2.0f * 3.14159f / 180.0f) * (HudDistance) * Correction;
      // float HudHeight = 2.0f * tanf(vfov / 2.0f * 3.14159f / 180.0f) * (HudDistance) *
      // Correction;
      HudWidth = 2.0f * tanf(((hfov / 2.0f) * Common::PI / 180.0f)) * HudDistance *
                 (AimDistance + CameraForward) / AimDistance;
      HudHeight = 2.0f * tanf(((vfov / 2.0f) * Common::PI / 180.0f)) * HudDistance *
                  (AimDistance + CameraForward) / AimDistance;
    }
    if (kind == 0 || bShowAim)
      HudThickness = HudWidth;

    float scale[3];  // width, height, and depth of box in game units divided by 2D width, height,
                     // and depth
    float position[3];  // position of front of box relative to the camera, in game units

    float viewport_scale[2];
    float viewport_offset[2];  // offset as a fraction of the viewport's width
    if (!bIsHudElement && !bIsOffscreen)
    {
      viewport_scale[0] = 1.0f;
      viewport_scale[1] = 1.0f;
      viewport_offset[0] = 0.0f;
      viewport_offset[1] = 0.0f;
    }
    else
    {
      Viewport& v = xfmem.viewport;
      float left, top, width, height;
      left = v.xOrig - v.wd - 342;
      top = v.yOrig + v.ht - 342;
      width = 2 * v.wd;
      height = -2 * v.ht;
      float screen_width = (float)GetFinalScreenRegion().GetWidth();
      float screen_height = (float)GetFinalScreenRegion().GetHeight();
      viewport_scale[0] = width / screen_width;
      viewport_scale[1] = height / screen_height;
      viewport_offset[0] = ((left + (width / 2)) - (0 + (screen_width / 2))) / screen_width;
      viewport_offset[1] = -((top + (height / 2)) - (0 + (screen_height / 2))) / screen_height;
    }

    // 3D HUD elements (may be part of 2D screen or HUD)
    if (bIsPerspective)
    {
      // these are the edges of the near clipping plane in game coordinates
      float left2D = 0;
      float right2D = 1;
      float bottom2D = 1;
      float top2D = 0;
      float zFar2D = 1;
      float zNear2D = -1;
      float zObj = zNear2D + (zFar2D - zNear2D) * g_ActiveConfig.fHud3DCloser;

      left2D *= zObj;
      right2D *= zObj;
      bottom2D *= zObj;
      top2D *= zObj;

      // Scale the width and height to fit the HUD in metres
      if (right2D == left2D)
      {
        scale[0] = 0;
      }
      else
      {
        scale[0] = viewport_scale[0] * HudWidth / (right2D - left2D);
      }
      if (top2D == bottom2D)
      {
        scale[1] = 0;
      }
      else
      {
        scale[1] = viewport_scale[1] * HudHeight /
                   (top2D - bottom2D);  // note that positive means up in 3D
      }
      // Keep depth the same scale as width, so it looks like a real object
      if (zFar2D == zNear2D)
      {
        scale[2] = scale[0];
      }
      else
      {
        scale[2] = scale[0];
      }
      // Adjust the position for off-axis projection matrices, and shifting the 2D screen
      position[0] = scale[0] * (-(right2D + left2D) / 2.0f) +
                    viewport_offset[0] * HudWidth;  // shift it right into the centre of the view
      position[1] = scale[1] * (-(top2D + bottom2D) / 2.0f) + viewport_offset[1] * HudHeight +
                    HudUp;  // shift it up into the centre of the view;
      // Shift it from the old near clipping plane to the HUD distance, and shift the camera forward
      if (!bHasWidest)
        position[2] = scale[2] * zObj - HudDistance;
      else
        position[2] = scale[2] * zObj - HudDistance;  // - CameraForward;
    }
    // 2D layer, or 2D viewport (may be part of 2D screen or HUD)
    else
    {
      float left2D = 0;
      float right2D = 1;
      float bottom2D = 1;
      float top2D = 0;
      float zNear2D = -1;
      float zFar2D = 1;

      // for 2D, work out the fraction of the HUD we should fill, and multiply the scale by that
      // also work out what fraction of the height we should shift it up, and what fraction of the
      // width we should shift it left
      // only multiply by the extra scale after adjusting the position?

      if (right2D == left2D)
      {
        scale[0] = 0;
      }
      else
      {
        scale[0] = viewport_scale[0] * HudWidth / (right2D - left2D);
      }
      if (top2D == bottom2D)
      {
        scale[1] = 0;
      }
      else
      {
        scale[1] = viewport_scale[1] * HudHeight /
                   (top2D - bottom2D);  // note that positive means up in 3D
      }
      if (zFar2D == zNear2D)
      {
        scale[2] = 0;  // The 2D layer was flat, so we make it flat instead of a box to avoid
                       // dividing by zero
      }
      else
      {
        scale[2] = HudThickness /
                   (zFar2D -
                    zNear2D);  // Scale 2D z values into 3D game units so it is the right thickness
      }
      position[0] = scale[0] * (-(right2D + left2D) / 2.0f) +
                    viewport_offset[0] * HudWidth;  // shift it right into the centre of the view
      position[1] = scale[1] * (-(top2D + bottom2D) / 2.0f) + viewport_offset[1] * HudHeight +
                    HudUp;  // shift it up into the centre of the view;
      // Shift it from the zero plane to the HUD distance, and shift the camera forward
      if (!bHasWidest)
        position[2] = -HudDistance;
      else
        position[2] = -HudDistance;  // - CameraForward;
    }

    Common::Matrix44 scale_matrix, position_matrix;
    // Common::Matrix44::Scale(scale_matrix, scale);
    {
      Common::Matrix33 m33_scale = Common::Matrix33::Scale(Common::Vec3(scale[0], scale[1], scale[2]));
      scale_matrix = Common::Matrix44::FromMatrix33(m33_scale);
    }
    position_matrix = Common::Matrix44::Translate(Common::Vec3(position[0], position[1], position[2]));

    // Common::Matrix44::InvertScale(scale_matrix);
    {
      // Check for zero scale factors to avoid division by zero.
      float inv_x = (scale[0] == 0.0f || scale[0] == -0.0f) ? 0.0f : 1.0f / scale[0];
      float inv_y = (scale[1] == 0.0f || scale[1] == -0.0f) ? 0.0f : 1.0f / scale[1];
      float inv_z = (scale[2] == 0.0f || scale[2] == -0.0f) ? 0.0f : 1.0f / scale[2];
      Common::Matrix33 m33_scale_inv = Common::Matrix33::Scale(Common::Vec3(inv_x, inv_y, inv_z));
      scale_matrix = Common::Matrix44::FromMatrix33(m33_scale_inv);
    }
    position_matrix = Common::Matrix44::Translate(Common::Vec3(-position_matrix.data[3], -position_matrix.data[7], -position_matrix.data[11]));
    invert_rotation_part(camera_pitch_matrix);
    invert_rotation_part(lean_back_matrix);
    invert_rotation_part(rotation_matrix);
    camera_position_matrix = Common::Matrix44::Translate(Common::Vec3(-camera_position_matrix.data[3], -camera_position_matrix.data[7], -camera_position_matrix.data[11]));
    free_look_matrix = Common::Matrix44::Translate(Common::Vec3(-free_look_matrix.data[3], -free_look_matrix.data[7], -free_look_matrix.data[11]));
    head_position_matrix = Common::Matrix44::Translate(Common::Vec3(-head_position_matrix.data[3], -head_position_matrix.data[7], -head_position_matrix.data[11]));

    // invert order: position, scale
    look_matrix = rotation_matrix * head_position_matrix * lean_back_matrix * free_look_matrix *
                  camera_pitch_matrix * camera_position_matrix * position_matrix * scale_matrix;
  }
  // done
  return true;
}
