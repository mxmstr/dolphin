// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/VertexShaderManager.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <algorithm> // For std::replace

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Matrix.h"
#include "Core/FreeLookConfig.h"
#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/FreeLookCamera.h"
#include "VideoCommon/GeometryShaderManager.h" // Needed for I_STEREOPARAMS
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VR.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"
#include "Core/System.h"

// For DEGREES_TO_RADIANS, etc.
#include "Common/MathUtil.h"

// TODO: Consider moving VR-specific globals (g_fProjectionMatrix, s_locked_skybox_orientation, etc.)
// into a VR state structure or making them static members of VertexShaderManager if appropriate,
// instead of file-scope statics, for better encapsulation in the future.

extern Common::Matrix44 g_head_tracking_matrix;
extern Common::Vec3 g_head_tracking_position;

ViewportType g_viewport_type = ViewportType::VIEW_FULLSCREEN;
ViewportType g_old_viewport_type = ViewportType::VIEW_FULLSCREEN;

// VR statistics / state tracking globals
//float vr_widest_3d_HFOV = 0.0f;
//float vr_widest_3d_VFOV = 0.0f;
//float vr_widest_3d_zNear = 0.0f;
//float vr_widest_3d_zFar = 0.0f;
// int vr_widest_3d_projNum = -1; // If debug logging with projNum is ported

// Game camera stabilization globals (set by CheckOrientationConstants)
//Common::Matrix44 g_game_camera_rotmat = Common::Matrix44::Identity();
//float g_game_camera_pos[3] = {0.0f, 0.0f, 0.0f};
extern bool g_vr_had_3D_already;

namespace // Anonymous namespace for VR helpers and related statics
{

// --- Start of VR-Hydra Ported Globals (file-scope static) ---
// g_fProjectionMatrix is the game's original projection matrix, potentially modified by VR logic for HUDs etc.
static std::array<float, 16> g_fProjectionMatrix;
static Common::Matrix33 s_locked_skybox_orientation;
static bool s_had_skybox = false;

// Viewport related globals
MathUtil::Rectangle<int> g_final_screen_region = MathUtil::Rectangle<int>(0, 0, EFB_WIDTH, EFB_HEIGHT);
MathUtil::Rectangle<int> g_requested_viewport = MathUtil::Rectangle<int>(0, 0, EFB_WIDTH, EFB_HEIGHT);
MathUtil::Rectangle<int> g_rendered_viewport = MathUtil::Rectangle<int>(0, 0, EFB_WIDTH, EFB_HEIGHT);
//SplitScreenType g_splitscreen_type = SplitScreenType;
//SplitScreenType g_old_splitscreen_type = SplitScreenType::Fullscreen;
bool g_is_skybox = false;


// TODO: Game specific globals (example, Metroid Prime)
// MetroidLayer g_metroid_layer = MetroidLayer::METROID_UNKNOWN;
// bool g_is_nes = false;
// --- End of VR-Hydra Ported Globals ---


// Projection Hack related structures and functions
struct ProjectionHack
{
  float sign;
  float value;
  ProjectionHack() : sign(0.0f), value(0.0f) {}
  ProjectionHack(float new_sign, float new_value) : sign(new_sign), value(new_value) {}
};
static ProjectionHack g_proj_hack_near;
static ProjectionHack g_proj_hack_far;

static float PHackValue(const std::string& sValue)
{
  float f = 0;
  bool fp = false;
  try {
    std::string temp_sValue = sValue;
    std::replace(temp_sValue.begin(), temp_sValue.end(), ',', '.');
    f = std::stof(temp_sValue);
    fp = temp_sValue.find('.') != std::string::npos;
  } catch (const std::invalid_argument& ia) {
    ERROR_LOG_FMT(VIDEO, "Invalid argument for PHackValue: {}", sValue.c_str());
    return 0.0f;
  } catch (const std::out_of_range& oor) {
    ERROR_LOG_FMT(VIDEO, "Out of range for PHackValue: {}", sValue.c_str());
    return 0.0f;
  }
  if (!fp && sValue.length() > 0)
    f /= 0xF4240;
  return f;
}

void UpdateProjectionHack(const ProjectionHackConfig& config)
{
  float near_value = 0, far_value = 0;
  float near_sign = 1.0, far_sign = 1.0;
  if (config.m_enable)
  {
    if (config.m_sznear) near_sign *= -1.0f;
    if (config.m_szfar) far_sign *= -1.0f;
    near_value = PHackValue(config.m_znear);
    far_value = PHackValue(config.m_zfar);
  }
  g_proj_hack_near = ProjectionHack(near_sign, near_value);
  g_proj_hack_far = ProjectionHack(far_sign, far_value);
}

void ScaleRequestedToRendered(const MathUtil::Rectangle<int>& requested, MathUtil::Rectangle<int>& rendered)
{
  if (g_requested_viewport.GetWidth() == 0 || g_requested_viewport.GetHeight() == 0) {
      rendered = requested;
      return;
  }
  float m_w = static_cast<float>(g_rendered_viewport.GetWidth()) / g_requested_viewport.GetWidth();
  rendered.left = static_cast<int>(0.5f + (requested.left - g_requested_viewport.left) * m_w + g_rendered_viewport.left);
  rendered.right = static_cast<int>(0.5f + (requested.right - g_requested_viewport.left) * m_w + g_rendered_viewport.left);
  float m_h = static_cast<float>(g_rendered_viewport.GetHeight()) / g_requested_viewport.GetHeight();
  rendered.top = static_cast<int>(0.5f + (requested.top - g_requested_viewport.top) * m_h + g_rendered_viewport.top);
  rendered.bottom = static_cast<int>(0.5f + (requested.bottom - g_requested_viewport.top) * m_h + g_rendered_viewport.bottom);
}

// Logging stubs
//static void ClearDebugProj() { /* Stub */ }
//static void DoLogViewport(int j, const Viewport& v, ViewportType type) { /* Stub */ }
//static void DoLogProj(int j, const float p[], const char* layer_name_str, ProjectionType proj_type) { /* Stub */ }

static void LogProj(const float p[], ProjectionType proj_type) {
    // TODO: Port game-specific layer detection (e.g., VR::GetGameSpecificLayerInfo) if used.
    // TODO: Update vr_widest_3d_HFOV, etc. if this logic is fully ported.
    // Example snippet for updating widest FOV:
    /*
    if (proj_type == ProjectionType::Perspective) {
      bool is_n64_style = (p[0] == 1.0f && p[1] == 1.0f && p[2] == 1.0f && p[3] == 1.0f);
      if (!is_n64_style && p[0] != 0.0f) {
         float hfov = 2.0f * atanf(1.0f / p[0]) * RADIANS_TO_DEGREES;
         float vfov = (p[2] != 0.0f) ? 2.0f * atanf(1.0f / p[2]) * RADIANS_TO_DEGREES : 0.0f;
         float n = 0.0f, f = 0.0f;
         if (std::abs(p[4]-1.0f) > 1e-6 && std::abs(p[4]+1.0f) > 1e-6) {
             n = p[5] / (p[4] - 1.0f); f = p[5] / (p[4] + 1.0f);
         }
         bool is_square_ish = std::abs(std::abs(p[0]) - std::abs(p[2])) < 0.1f * std::abs(p[0]);
         if (std::abs(hfov) > vr_widest_3d_HFOV && std::abs(hfov) <= 125.0f && (!is_square_ish || is_n64_style)) {
             vr_widest_3d_HFOV = std::abs(hfov); vr_widest_3d_VFOV = std::abs(vfov);
             vr_widest_3d_zNear = std::abs(n); vr_widest_3d_zFar = std::abs(f);
         }
      }
    }
    */
}
static void LogViewport(const Viewport& v, ViewportType type) { /* Stub */ }

const char* GetViewportTypeName(ViewportType v)
{
  if (g_is_skybox) return "Skybox";
  switch (v)
  {
    case ViewportType::VIEW_FULLSCREEN: return "Fullscreen";
    case ViewportType::VIEW_LETTERBOXED: return "Letterboxed";
    case ViewportType::VIEW_HUD_ELEMENT: return "HUD element";
    case ViewportType::VIEW_OFFSCREEN: return "Offscreen";
    case ViewportType::VIEW_RENDER_TO_TEXTURE: return "Render to Texture";
    case ViewportType::VIEW_PLAYER_1: return "Player 1";
    case ViewportType::VIEW_PLAYER_2: return "Player 2";
    case ViewportType::VIEW_PLAYER_3: return "Player 3";
    case ViewportType::VIEW_PLAYER_4: return "Player 4";
    default: return "Error";
  }
}

void SetViewportType(const Viewport& v)
{
  g_old_viewport_type = g_viewport_type;
  float left = v.xOrig - v.wd;
  float top = v.yOrig - v.ht;
  float width = 2.0f * v.wd;
  float height = 2.0f * v.ht;

  const float screen_width = static_cast<float>(g_framebuffer_manager->GetEFBWidth());
  const float screen_height = static_cast<float>(g_framebuffer_manager->GetEFBHeight());
  float min_screen_width = 0.90f * screen_width;
  float min_screen_height = 0.90f * screen_height;
  float max_top_offset = screen_height - min_screen_height;
  float max_left_offset = screen_width - min_screen_width;

  if (width == height && (width == 1 || width == 2 || width == 4 || (static_cast<int>(width) % 8 == 0)) &&
      (left == 0 || top == 0 || std::abs(top - (screen_height - height)) < 1.0f || std::abs(left - (screen_width - width)) < 1.0f ) &&
      !(width == 512 && screen_width == 512 && screen_height == 512)) {
    g_viewport_type = ViewportType::VIEW_RENDER_TO_TEXTURE;
  } else if (width >= min_screen_width && std::abs(left) <= max_left_offset) {
    if (height >= min_screen_height && std::abs(top) <= max_top_offset) {
        g_viewport_type = (width == screen_width && height == screen_height && left == 0 && top == 0) ?
                           ViewportType::VIEW_FULLSCREEN : ViewportType::VIEW_LETTERBOXED;
    } else if (height >= min_screen_height / 2.1f && height <= screen_height / 1.9f) {
        if (std::abs(top) <= max_top_offset) g_viewport_type = ViewportType::VIEW_PLAYER_1;
        else if (std::abs(top - height) <= max_top_offset) g_viewport_type = ViewportType::VIEW_PLAYER_2;
        else g_viewport_type = ViewportType::VIEW_LETTERBOXED;
    } else {
      g_viewport_type = ViewportType::VIEW_LETTERBOXED;
    }
  } else if (height >= min_screen_height && std::abs(top) <= max_top_offset) {
     if (width >= min_screen_width / 2.1f && width <= screen_width / 1.9f) {
        if (std::abs(left) <= max_left_offset) g_viewport_type = ViewportType::VIEW_PLAYER_1;
        else if (std::abs(left - width) <= max_left_offset) g_viewport_type = ViewportType::VIEW_PLAYER_2;
        else g_viewport_type = ViewportType::VIEW_HUD_ELEMENT;
     } else {
        g_viewport_type = ViewportType::VIEW_LETTERBOXED;
     }
  } else if (width >= (min_screen_width / 2.1f) && width <= (screen_width / 1.9f) &&
           height >= (min_screen_height / 2.1f) && height <= (screen_height / 1.9f)) {
      if(std::abs(left) <= max_left_offset && std::abs(top) <= max_top_offset) g_viewport_type = ViewportType::VIEW_PLAYER_1;
      else if(std::abs(left-width) <= max_left_offset && std::abs(top) <= max_top_offset) g_viewport_type = ViewportType::VIEW_PLAYER_2;
      else if(std::abs(left) <= max_left_offset && std::abs(top-height) <= max_top_offset) g_viewport_type = ViewportType::VIEW_PLAYER_3;
      else if(std::abs(left-width) <= max_left_offset && std::abs(top-height) <= max_top_offset) g_viewport_type = ViewportType::VIEW_PLAYER_4;
      else g_viewport_type = ViewportType::VIEW_HUD_ELEMENT;
  } else if (left >= screen_width || top >= screen_height || left + width <= 0 || top + height <= 0) {
    g_viewport_type = ViewportType::VIEW_OFFSCREEN;
  } else {
    g_viewport_type = ViewportType::VIEW_HUD_ELEMENT;
  }

  if (g_viewport_type == ViewportType::VIEW_FULLSCREEN || g_viewport_type == ViewportType::VIEW_LETTERBOXED ||
      (g_viewport_type >= ViewportType::VIEW_PLAYER_1 && g_viewport_type <= ViewportType::VIEW_PLAYER_4)) {
    float znear_norm = (v.farZ - v.zRange) / 16777216.0f;
    float zfar_norm = v.farZ / 16777216.0f;
    g_is_skybox = (znear_norm >= 0.99f && zfar_norm >= 0.999f);
  } else {
    g_is_skybox = false;
  }
}

void CheckOrientationConstants()
{
  g_game_camera_rotmat = Common::Matrix44::Identity();
  g_game_camera_pos[0] = g_game_camera_pos[1] = g_game_camera_pos[2] = 0.0f;
  // TODO: Full original VR-Hydra CheckOrientationConstants logic (highly game-specific)
  // involves reading game memory or using heuristics on constants.posnormalmatrix.
  // This is a complex piece requiring per-game profiles or advanced heuristics.
}

void CheckSkybox()
{
  if (xfmem.projection.type == ProjectionType::Perspective) {
    const float* pnm_data = VertexShaderManager::constants.posnormalmatrix[0].data();
    if (pnm_data[3] == 0.0f && pnm_data[7] == 0.0f && pnm_data[11] == 0.0f) {
      bool is_identity_scale_rot = (pnm_data[0] == 1.0f && pnm_data[1] == 0.0f && pnm_data[2] == 0.0f &&
                                    pnm_data[4] == 0.0f && pnm_data[5] == 1.0f && pnm_data[6] == 0.0f &&
                                    pnm_data[8] == 0.0f && pnm_data[9] == 0.0f && pnm_data[10] == 1.0f);
      if (!is_identity_scale_rot) g_is_skybox = true;
    }
  }
}

void LockSkybox()
{
  if (xfmem.projection.type == ProjectionType::Perspective && g_is_skybox && g_ActiveConfig.iMotionSicknessSkybox == 2) {
    float current_skybox_modelview_rot_scale[3*3];
    const float* pnm_data = VertexShaderManager::constants.posnormalmatrix[0].data();
    current_skybox_modelview_rot_scale[0] = pnm_data[0]; current_skybox_modelview_rot_scale[1] = pnm_data[1]; current_skybox_modelview_rot_scale[2] = pnm_data[2];
    current_skybox_modelview_rot_scale[3] = pnm_data[4]; current_skybox_modelview_rot_scale[4] = pnm_data[5]; current_skybox_modelview_rot_scale[5] = pnm_data[6];
    current_skybox_modelview_rot_scale[6] = pnm_data[8]; current_skybox_modelview_rot_scale[7] = pnm_data[9]; current_skybox_modelview_rot_scale[8] = pnm_data[10];

    if (s_had_skybox) {
      // TODO: This direct modification of 'constants' is a hack.
      // A cleaner way might be needed if XF memory is the sole source of truth before shader upload.
      float* target_pnm = VertexShaderManager::constants.posnormalmatrix[0].data();
      target_pnm[0] = s_locked_skybox_orientation.data[0]; target_pnm[1] = s_locked_skybox_orientation.data[1]; target_pnm[2] = s_locked_skybox_orientation.data[2];
      target_pnm[4] = s_locked_skybox_orientation.data[3]; target_pnm[5] = s_locked_skybox_orientation.data[4]; target_pnm[6] = s_locked_skybox_orientation.data[5];
      target_pnm[8] = s_locked_skybox_orientation.data[6]; target_pnm[9] = s_locked_skybox_orientation.data[7]; target_pnm[10] = s_locked_skybox_orientation.data[8];
      VertexShaderManager::dirty = true;
    } else {
      memcpy(&s_locked_skybox_orientation.data, current_skybox_modelview_rot_scale, sizeof(s_locked_skybox_orientation.data));
      s_had_skybox = true;
    }
  }
}

static void CalculateVRProjectionViewMatrices(
    const float* rawProjection, ProjectionType projection_type, float UnitsPerMetre,
    bool bStuckToHead, bool bHideLeft, bool bHideRight, bool bNoForward, int iTelescopeHack,
    float fLeftWidthHack, float fLeftHeightHack, float fRightWidthHack, float fRightHeightHack,
    float fSharedRightHack, float fSharedUpHack, bool bN64,
    Common::Matrix44& out_final_matrix_left, Common::Matrix44& out_final_matrix_right,
    std::array<float, 4>& out_stereoparams)
{
  float znear_game, zfar_game, hfov_game, vfov_game;
  float zoom_forward = 0.0f;

  if (vr_widest_3d_HFOV <= g_ActiveConfig.fMinFOV && vr_widest_3d_HFOV > 0.0f && iTelescopeHack <= 0) {
    float tan_min_fov_half = tanf(DEGREES_TO_RADIANS(g_ActiveConfig.fMinFOV) / 2.0f);
    float tan_widest_fov_half = tanf(DEGREES_TO_RADIANS(vr_widest_3d_HFOV) / 2.0f);
    if (tan_widest_fov_half != 0.0f) {
        zoom_forward = g_ActiveConfig.fAimDistance * tan_min_fov_half / tan_widest_fov_half;
        zoom_forward -= g_ActiveConfig.fAimDistance;
    }
  }

  if (projection_type == ProjectionType::Perspective && g_viewport_type != ViewportType::VIEW_HUD_ELEMENT && g_viewport_type != ViewportType::VIEW_OFFSCREEN) {
    const float* p_raw = rawProjection;
    if (std::abs(p_raw[4] - 1.0f) > 1e-6f && std::abs(p_raw[4] + 1.0f) > 1e-6f) {
         znear_game = p_raw[5] / (p_raw[4] - 1.0f); zfar_game  = p_raw[5] / (p_raw[4] + 1.0f);
    } else { znear_game = 0.1f * UnitsPerMetre; zfar_game = 1000.0f * UnitsPerMetre; }
    if (znear_game >= zfar_game || znear_game <= 0.0f) {
        znear_game = 0.1f * UnitsPerMetre; if (zfar_game <= znear_game) zfar_game = znear_game + 100.0f * UnitsPerMetre;
    }
    hfov_game = (p_raw[0] != 0.0f) ? 2.0f * RADIANS_TO_DEGREES(atanf(1.0f / p_raw[0])) : 90.0f;
    vfov_game = (p_raw[2] != 0.0f) ? 2.0f * RADIANS_TO_DEGREES(atanf(1.0f / p_raw[2])) : hfov_game * ((static_cast<float>(g_framebuffer_manager->GetEFBWidth()) / static_cast<float>(g_framebuffer_manager->GetEFBHeight())) > 1.5f ? (9.0f/16.0f) : (3.0f/4.0f));
  } else {
    if (vr_widest_3d_HFOV > 0.0f) {
      znear_game = vr_widest_3d_zNear; zfar_game = vr_widest_3d_zFar;
      hfov_game = (zoom_forward != 0.0f) ? g_ActiveConfig.fMinFOV : vr_widest_3d_HFOV;
      vfov_game = (vr_widest_3d_HFOV > 0.0f) ? hfov_game * (vr_widest_3d_VFOV / vr_widest_3d_HFOV) : hfov_game * ((static_cast<float>(g_framebuffer_manager->GetEFBWidth()) / static_cast<float>(g_framebuffer_manager->GetEFBHeight())) > 1.5f ? (9.0f/16.0f) : (3.0f/4.0f));
    } else {
      znear_game = 0.5f * UnitsPerMetre; zfar_game = 40.0f * UnitsPerMetre; hfov_game = 70.0f;
      vfov_game = hfov_game * ((static_cast<float>(g_framebuffer_manager->GetEFBWidth()) / static_cast<float>(g_framebuffer_manager->GetEFBHeight())) > 1.5f ? (9.0f/16.0f) : (3.0f/4.0f));
    }
  }

  Common::Matrix44 proj_left_hmd, proj_right_hmd;
  VR_GetProjectionMatrices(proj_left_hmd, proj_right_hmd, znear_game, zfar_game);

  proj_left_hmd.data[0] *= fLeftWidthHack;   proj_left_hmd.data[5] *= fLeftHeightHack;
  proj_left_hmd.data[8] -= fSharedRightHack; proj_left_hmd.data[9] -= fSharedUpHack;
  proj_right_hmd.data[0] *= fRightWidthHack; proj_right_hmd.data[5] *= fRightHeightHack;
  proj_right_hmd.data[8] -= fSharedRightHack; proj_right_hmd.data[9] -= fSharedUpHack;

  out_stereoparams[0] = proj_left_hmd.data[0];  out_stereoparams[1] = proj_right_hmd.data[0];
  out_stereoparams[2] = proj_left_hmd.data[8];  out_stereoparams[3] = proj_right_hmd.data[8];
  if (g_backend_info.bSupportsGeometryShaders) { proj_left_hmd.data[8] = 0.0f; }

  Common::Matrix44 mat_cam_stab_pos = Common::Matrix44::Identity(), mat_cam_stab_rot = Common::Matrix44::Identity();
  Common::Matrix44 mat_cam_fwd = Common::Matrix44::Identity(), mat_cam_pitch_cfg = Common::Matrix44::Identity();
  Common::Matrix44 mat_freelook_view = Common::Matrix44::Identity();
  Common::Matrix44 mat_lean = Common::Matrix44::Identity();

  if (!bStuckToHead) {
    if (g_freelook_camera.IsActive() && g_freelook_camera.GetController() && g_freelook_camera.GetController()->SupportsInput()) {
        mat_freelook_view = g_freelook_camera.GetController()->GetView();
        // Note: UnitsPerMetre scaling for freelook position needs to be handled by the FPSCamera controller's GetView()
        // or its internal speed settings if GetView() provides world-to-camera directly.
        // The original code scaled the position by UnitsPerMetre when constructing mat_fl_pos.
        // If GetView() doesn't account for UnitsPerMetre, this might need adjustment.
        // For now, we assume GetView() is correctly scaled or that speed settings achieve this.
    }
    if (g_ActiveConfig.bCanReadCameraAngles) {
        if (g_ActiveConfig.bStabilizeX || g_ActiveConfig.bStabilizeY || g_ActiveConfig.bStabilizeZ)
            mat_cam_stab_pos = Common::Matrix44::Translate(Common::Vec3(-g_game_camera_pos[0], -g_game_camera_pos[1], -g_game_camera_pos[2]) * UnitsPerMetre);
        if (g_ActiveConfig.bStabilizePitch || g_ActiveConfig.bStabilizeRoll || g_ActiveConfig.bStabilizeYaw)
            mat_cam_stab_rot = g_game_camera_rotmat.Inverted();
    }
    float current_cam_pitch_deg = (projection_type == ProjectionType::Perspective || vr_widest_3d_HFOV > 0) ? g_ActiveConfig.fCameraPitch : g_ActiveConfig.fScreenPitch;
    mat_cam_pitch_cfg = Common::Matrix44::RotateX(DEGREES_TO_RADIANS(-current_cam_pitch_deg));
    if (!bNoForward && !g_is_skybox)
      mat_cam_fwd = Common::Matrix44::Translate({ 0.0f, 0.0f, (g_ActiveConfig.fCameraForward + zoom_forward) * UnitsPerMetre });
    mat_lean = Common::Matrix44::RotateX(DEGREES_TO_RADIANS(-g_ActiveConfig.fLeanBackAngle));
  }

  // The order is: World -> CamStabPos -> CamStabRot -> CamFwd -> CamPitchCfg -> FreeLook(View) -> Lean -> HeadPos -> HeadRot -> EyeOffset -> Projection
  // So, view_base_pre_head is everything before Head tracking and EyeOffset, applied to world coordinates.
  // It means: mat_lean * mat_freelook_view * mat_cam_pitch_cfg * mat_cam_fwd * mat_cam_stab_rot * mat_cam_stab_pos
  Common::Matrix44 view_base_pre_head = mat_lean * mat_freelook_view * mat_cam_pitch_cfg * mat_cam_fwd * mat_cam_stab_rot * mat_cam_stab_pos;

  if (!(projection_type == ProjectionType::Perspective && g_viewport_type != ViewportType::VIEW_HUD_ELEMENT && g_viewport_type != ViewportType::VIEW_OFFSCREEN)) {
    // TODO: Port detailed HUD transformation logic from VR-Hydra.
    // This involves creating a specific model matrix for the HUD plane/box.
  }

  Common::Matrix44 mat_head_pos_vr = Common::Matrix44::Identity();
  Common::Matrix44 mat_head_rot_vr = Common::Matrix44::Identity();
  if (!bStuckToHead) {
      // VR::VR_UpdateHeadTrackingIfNeeded() is called from SetConstants before this helper
      if (g_ActiveConfig.bOrientationTracking) { mat_head_rot_vr = g_head_tracking_matrix; }
      if (g_ActiveConfig.bPositionTracking) { mat_head_pos_vr = Common::Matrix44::Translate(g_head_tracking_position * UnitsPerMetre); }
  }
  Common::Matrix44 world_to_head_view = mat_head_rot_vr * mat_head_pos_vr * view_base_pre_head;

  Common::Matrix44 eye_offset_left_mat = Common::Matrix44::Identity();
  Common::Matrix44 eye_offset_right_mat = Common::Matrix44::Identity();
  if (!g_is_skybox) {
      Common::Vec3 eye_pos_left_offset, eye_pos_right_offset;
      float eye_pos_left_raw[3];
      float eye_pos_right_raw[3];
      VR_GetEyePos(eye_pos_left_raw, eye_pos_right_raw);
      eye_pos_left_offset = Common::Vec3(eye_pos_left_raw[0], eye_pos_left_raw[1], eye_pos_left_raw[2]);
      eye_pos_right_offset = Common::Vec3(eye_pos_right_raw[0], eye_pos_right_raw[1], eye_pos_right_raw[2]);
      eye_offset_left_mat = Common::Matrix44::Translate(eye_pos_left_offset * UnitsPerMetre);
      eye_offset_right_mat = Common::Matrix44::Translate(eye_pos_right_offset * UnitsPerMetre);
  }
  Common::Matrix44 world_to_eye_left_view = eye_offset_left_mat * world_to_head_view;
  Common::Matrix44 world_to_eye_right_view = eye_offset_right_mat * world_to_head_view;

  out_final_matrix_left = proj_left_hmd * world_to_eye_left_view;
  out_final_matrix_right = proj_right_hmd * world_to_eye_right_view;

  // TODO: Implement N64 specific transformation adjustments if required (if bN64 is true).
  // TODO: Implement projection flipping logic if rawProjection indicates it (flipped_x/flipped_y).

  if (bHideLeft) out_final_matrix_left = Common::Matrix44::Zero();
  if (bHideRight) out_final_matrix_right = Common::Matrix44::Zero();
}

} // Anonymous namespace for VR helpers and related statics


// Define static members
VertexShaderConstants VertexShaderManager::constants;
float4 VertexShaderManager::constants_eye_projection[2][4]; // For VR stereo
bool VertexShaderManager::m_layer_on_top = false; // For VR HUD layering
bool VertexShaderManager::dirty = true; // Initialize dirty to true


// VR view manipulation functions
// These now primarily signal that the projection needs recalculation due to view changes.
// The actual view transformation is handled by g_freelook_camera and VR head tracking.
// VR-Hydra had its own s_fViewTranslationVector and s_fViewRotation.
// We now rely on g_freelook_camera for freelook and VR system for head tracking.
// These functions will ensure VertexShaderManager::dirty is set.
void VertexShaderManager::ScaleView(float scale)
{
  // Inform FreeLookCamera or VR system about scaling if necessary,
  // For now, primarily mark projection dirty.
  // In Hydra, this scaled s_fViewTranslationVector. If freelook camera needs explicit scaling of its movement speed
  // or position due to world scale changes, that would happen via g_freelook_camera.
  // For example: g_freelook_camera.ScaleMovement(scale);
  // The controller will set its own dirty flag if its properties change.
  // VertexShaderManager::dirty is for VSM's constants.
  VertexShaderManager::dirty = true; // Our projection constants depend on it
}

void VertexShaderManager::TranslateView(float left_metres, float forward_metres, float down_metres)
{
  // These are inputs for the FreeLook camera to move.
  // translation_cam_space was {left_metres, -down_metres, -forward_metres};
  // MoveHorizontal for x, MoveVertical for y, MoveForward for z.
  // Controller's local axes: +X right, +Y up, +Z backward.
  // Input: left_metres (+left), forward_metres (+forward), down_metres (+down)
  if (g_freelook_camera.GetController() && g_freelook_camera.GetController()->SupportsInput())
  {
    auto* input_controller = static_cast<CameraControllerInput*>(g_freelook_camera.GetController());
    // FPSCamera::MoveHorizontal: +amt moves camera right. Input left_metres: +val means move left.
    input_controller->MoveHorizontal(-left_metres);
    // FPSCamera::MoveVertical: +amt moves camera up. Input down_metres: +val means move down.
    input_controller->MoveVertical(-down_metres);
    // FPSCamera::MoveForward: +amt moves camera forward. Input forward_metres: +val means move forward.
    input_controller->MoveForward(forward_metres);
  }
  // The controller's Move methods should set its internal dirty flag.
  VertexShaderManager::dirty = true; // VSM constants depend on camera view
}

void VertexShaderManager::RotateView(float x_rad, float y_rad)
{
  // x_rad is yaw (around Y axis), y_rad is pitch (around X axis)
  // FPSCamera::Rotate likely expects {pitch, yaw, roll}
  if (g_freelook_camera.GetController() && g_freelook_camera.GetController()->SupportsInput())
  {
    auto* input_controller = static_cast<CameraControllerInput*>(g_freelook_camera.GetController());
    input_controller->Rotate({y_rad, x_rad, 0.0f});
  }
  // The controller's Rotate method should set its internal dirty flag.
  VertexShaderManager::dirty = true; // VSM constants depend on camera view
}

void VertexShaderManager::ResetView()
{
  // Attempting to replace g_freelook_camera.Reset() due to persistent compile error.
  // Replicating the logic from FreeLookCamera::Reset() here.
  if (g_freelook_camera.GetController() && g_freelook_camera.GetController()->SupportsInput())
  {
    // CameraControllerInput has a virtual Reset method.
    if (auto* input_controller = dynamic_cast<CameraControllerInput*>(g_freelook_camera.GetController()))
    {
      input_controller->Reset();
    }
  }
  // VR-Hydra also reset VRTracker::ResetView();
  // This would be VR::VR_ResetTrackerView(); or similar if such a function exists.
  // VR::VR_RecenterView() is a placeholder for the actual function name from VR.h/cpp
  // that should be called to reset/recenter the HMD's tracking origin or view.
  if (g_has_hmd) {
    VR_RecenterHMD();
  }
  s_had_skybox = false; // Reset skybox locking state
  g_vr_had_3D_already = false; // Reset 3D scene detection state
  vr_widest_3d_HFOV = 0.0f; // Reset widest FOV tracking
  // Reset other VR specific view states if necessary
  VertexShaderManager::dirty = true;
}


void VertexShaderManager::Init()
{
  m_projection_graphics_mod_change = false;
  constants = {};
  m_projection_matrix = Common::Matrix44::Identity().data;

  // Initialize VR specific members and globals
  memset(constants_eye_projection, 0, sizeof(constants_eye_projection));
  m_layer_on_top = false;
  //memset(g_fProjectionMatrix, 0, sizeof(g_fProjectionMatrix));
  // Set g_fProjectionMatrix to a default identity or suitable state if needed at init.
  g_fProjectionMatrix[0] = g_fProjectionMatrix[5] = g_fProjectionMatrix[10] = g_fProjectionMatrix[15] = 1.0f;


  s_locked_skybox_orientation = Common::Matrix33::Identity();
  s_had_skybox = false;

  g_viewport_type = ViewportType::VIEW_FULLSCREEN;
  g_old_viewport_type = ViewportType::VIEW_FULLSCREEN;
  g_game_camera_rotmat = Common::Matrix44::Identity();
  g_game_camera_pos[0] = g_game_camera_pos[1] = g_game_camera_pos[2] = 0.0f;
  g_vr_had_3D_already = false;
  vr_widest_3d_HFOV = 0.0f;
  vr_widest_3d_VFOV = 0.0f;
  vr_widest_3d_zNear = 0.0f;
  vr_widest_3d_zFar = 0.0f;

  ResetView();
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

void VertexShaderManager::SetProjectionMatrix(XFStateManager& xf_state_manager)
{
  if (xf_state_manager.DidProjectionChange() || g_freelook_camera.GetController()->IsDirty())
  {
    xf_state_manager.ResetProjection();
    auto corrected_matrix = LoadProjectionMatrix();
    memcpy(constants.projection.data(), corrected_matrix.data.data(), 16 * sizeof(float));
  }
}

bool VertexShaderManager::UseVertexDepthRange()
{
  if (!g_backend_info.bSupportsDepthClamp) return false;
  if (bpmem.ztex2.op != ZTexOp::Disabled && !bpmem.zcontrol.early_ztest) return true;
  if (!g_backend_info.bSupportsReversedDepthRange) {
    if (xfmem.viewport.zRange < 0.0f) return true;
    if (xfmem.viewport.zRange > xfmem.viewport.farZ) return true;
  }
  return std::abs(xfmem.viewport.zRange) > 16777215.0f || std::abs(xfmem.viewport.farZ) > 16777215.0f;
}

void VertexShaderManager::SetConstants(const std::vector<std::string>& textures,
                                       XFStateManager& xf_state_manager)
{
  GeometryShaderManager geometry_shader_manager = Core::System::GetInstance().GetGeometryShaderManager();
  bool position_changed = xf_state_manager.DidPosNormalChange() || xf_state_manager.GetPerVertexTransformMatrixChanges()[0] >=0;
  bool skybox_changed = false;

  VertexShaderManager::m_layer_on_top = false;
  bool bFullscreenLayer = false;
  bool bStuckToHead = false, bHide = false;
  int iTelescopeHack = -1;
  float fScaleHack = 1.0f, fWidthHack = 1.0f, fHeightHack = 1.0f, fUpHack = 0.0f, fRightHack = 0.0f;

  // TODO: Game-specific VR logic (e.g. VR::GetGameSpecificTweaks)

  bHide = bHide || (g_has_hmd && (g_viewport_type == ViewportType::VIEW_OFFSCREEN ||
                 (g_viewport_type >= ViewportType::VIEW_PLAYER_1 && g_viewport_type <= ViewportType::VIEW_PLAYER_4 &&
                  g_ActiveConfig.iVRPlayer != (static_cast<int>(g_viewport_type) - static_cast<int>(ViewportType::VIEW_PLAYER_1)))));
  bHide = bHide || (g_is_skybox && g_ActiveConfig.iMotionSicknessSkybox == 1) || g_vr_black_screen;

  float fLeftWidthHack = fWidthHack, fRightWidthHack = fWidthHack;
  float fLeftHeightHack = fHeightHack, fRightHeightHack = fHeightHack;
  bool bHideLeft = bHide, bHideRight = bHide, bNoForward = false;

  if (iTelescopeHack < 0 && g_ActiveConfig.iTelescopeEye != 0 &&
      vr_widest_3d_VFOV <= g_ActiveConfig.fTelescopeMaxFOV && vr_widest_3d_VFOV > 1.0f &&
      (g_ActiveConfig.fTelescopeMaxFOV <= g_ActiveConfig.fMinFOV ||
       (g_ActiveConfig.fTelescopeMaxFOV > g_ActiveConfig.fMinFOV && vr_widest_3d_VFOV > g_ActiveConfig.fMinFOV))) {
    iTelescopeHack = g_ActiveConfig.iTelescopeEye;
  }

  if (g_has_hmd && iTelescopeHack > 0) {
    bNoForward = true;
    // TODO: Full telescope scaling logic using VR::VR_GetProjectionHalfTan
  }

  if (xf_state_manager.DidViewportChange()) {
    xf_state_manager.ResetViewportChange();
    bool viewport_type_changed_old = (g_viewport_type != g_old_viewport_type);
    SetViewportType(xfmem.viewport);
    if (g_viewport_type != g_old_viewport_type || viewport_type_changed_old) {
        VertexShaderManager::dirty = true;
        skybox_changed = true;
    }
    const float pixel_center_correction = 7.0f / 12.0f - 0.5f;
    const bool bUseVertexRounding = g_ActiveConfig.UseVertexRounding();
    const float viewport_width_scaled = bUseVertexRounding ? (2.f * xfmem.viewport.wd) : g_framebuffer_manager->EFBToScaledXf(2.f * xfmem.viewport.wd);
    const float viewport_height_scaled = bUseVertexRounding ? (2.f * xfmem.viewport.ht) : g_framebuffer_manager->EFBToScaledXf(2.f * xfmem.viewport.ht);
    const float pixel_size_x = (viewport_width_scaled != 0.0f) ? 2.f / viewport_width_scaled : 0.0f;
    const float pixel_size_y = (viewport_height_scaled != 0.0f) ? 2.f / viewport_height_scaled : 0.0f;
    constants.pixelcentercorrection[0] = pixel_center_correction * pixel_size_x;
    constants.pixelcentercorrection[1] = pixel_center_correction * pixel_size_y;
    constants.pixelcentercorrection[2] = 1.0f; constants.pixelcentercorrection[3] = 0.0f;
    constants.viewport[0] = (2.f * xfmem.viewport.wd); constants.viewport[1] = (2.f * xfmem.viewport.ht);
    if (UseVertexDepthRange()) {
      if (g_backend_info.bSupportsReversedDepthRange) {
        constants.pixelcentercorrection[2] = std::abs(xfmem.viewport.zRange) / 16777215.0f;
        constants.pixelcentercorrection[3] = (xfmem.viewport.zRange < 0.0f) ? (xfmem.viewport.farZ / 16777215.0f) : (1.0f - xfmem.viewport.farZ / 16777215.0f);
      } else {
        constants.pixelcentercorrection[2] = xfmem.viewport.zRange / 16777215.0f;
        constants.pixelcentercorrection[3] = 1.0f - xfmem.viewport.farZ / 16777215.0f;
      }
    }
    dirty = true;
    BPFunctions::SetScissorAndViewport();
    g_stats.AddScissorRect();
  }

  if (position_changed && g_ActiveConfig.bDetectSkybox && !g_is_skybox) {
    CheckSkybox();
    if (g_is_skybox) { skybox_changed = true; VertexShaderManager::dirty = true; }
  }

  // Call CheckOrientationConstants once per frame if conditions met (e.g. if VR active and stabilization enabled)
  if (g_has_hmd && g_ActiveConfig.bEnableVR && g_ActiveConfig.bCanReadCameraAngles) {
      CheckOrientationConstants(); // Updates g_game_camera_pos/rotmat
      // If CheckOrientationConstants changes these, projection might need update
      // This depends on whether stabilization is active. For now, assume it makes things dirty.
      VertexShaderManager::dirty = true;
  }

  // Lock skybox if enabled and skybox detected
  if (g_is_skybox && g_ActiveConfig.iMotionSicknessSkybox == 2) {
      LockSkybox(); // May modify constants.posnormalmatrix and set dirty = true
  }


  std::vector<GraphicsModAction*> projection_actions_vec; // Renamed to avoid conflict
  if (g_ActiveConfig.bGraphicMods) {
    for (const auto& action : g_graphics_mod_manager->GetProjectionActions(xfmem.projection.type))
        projection_actions_vec.push_back(action);
    for (const auto& texture : textures) {
      for (const auto& action : g_graphics_mod_manager->GetProjectionTextureActions(xfmem.projection.type, texture))
        projection_actions_vec.push_back(action);
    }
  }

  if (xf_state_manager.DidProjectionChange() || g_freelook_camera.GetController()->IsDirty() ||
      !projection_actions_vec.empty() || m_projection_graphics_mod_change || VertexShaderManager::dirty) {
    xf_state_manager.ResetProjection();
    m_projection_graphics_mod_change = !projection_actions_vec.empty();

    const Projection::Raw rawProjection = xfmem.projection.rawProjection;
    switch (xfmem.projection.type) {
      case ProjectionType::Perspective:
        g_fProjectionMatrix[0]=rawProjection[0]; g_fProjectionMatrix[1]=0.0f; g_fProjectionMatrix[2]=rawProjection[1]; g_fProjectionMatrix[3]=0.0f;
        g_fProjectionMatrix[4]=0.0f; g_fProjectionMatrix[5]=rawProjection[2]; g_fProjectionMatrix[6]=rawProjection[3]; g_fProjectionMatrix[7]=0.0f;
        g_fProjectionMatrix[8]=0.0f; g_fProjectionMatrix[9]=0.0f; g_fProjectionMatrix[10]=rawProjection[4]; g_fProjectionMatrix[11]=rawProjection[5];
        g_fProjectionMatrix[12]=0.0f; g_fProjectionMatrix[13]=0.0f; g_fProjectionMatrix[14]=-1.0f; g_fProjectionMatrix[15]=0.0f;
        break;
      case ProjectionType::Orthographic:
        g_fProjectionMatrix[0]=rawProjection[0]; g_fProjectionMatrix[1]=0.0f; g_fProjectionMatrix[2]=0.0f; g_fProjectionMatrix[3]=rawProjection[1];
        g_fProjectionMatrix[4]=0.0f; g_fProjectionMatrix[5]=rawProjection[2]; g_fProjectionMatrix[6]=0.0f; g_fProjectionMatrix[7]=rawProjection[3];
        g_fProjectionMatrix[8]=0.0f; g_fProjectionMatrix[9]=0.0f;
        if (g_ActiveConfig.bEnableProjectionHack) {
            g_fProjectionMatrix[10]=(g_proj_hack_near.value+rawProjection[4])*((g_proj_hack_near.sign==0.0f)?1.0f:g_proj_hack_near.sign);
            g_fProjectionMatrix[11]=(g_proj_hack_far.value+rawProjection[5])*((g_proj_hack_far.sign==0.0f)?1.0f:g_proj_hack_far.sign);
        } else {
            g_fProjectionMatrix[10]=rawProjection[4]; g_fProjectionMatrix[11]=rawProjection[5];
        }
        g_fProjectionMatrix[12]=0.0f; g_fProjectionMatrix[13]=0.0f; g_fProjectionMatrix[14]=0.0f; g_fProjectionMatrix[15]=1.0f;
        break;
      default: ERROR_LOG_FMT(VIDEO, "Unknown projection type: {}", xfmem.projection.type);
    }
    LogProj(rawProjection.data(), xfmem.projection.type); // Update VR stats like widest FOV

    bool bN64 = (xfmem.projection.type == ProjectionType::Perspective && rawProjection[0] == 1.0f && rawProjection[1] == 1.0f && rawProjection[2] == 1.0f && rawProjection[3] == 1.0f);
    float UnitsPerMetre = g_ActiveConfig.fUnitsPerMetre * fScaleHack / g_ActiveConfig.fScale;
    bFullscreenLayer = (g_ActiveConfig.bHudFullscreen && xfmem.projection.type != ProjectionType::Perspective && g_viewport_type != ViewportType::VIEW_RENDER_TO_TEXTURE);


    if (bHide) {
      // constants.projection is std::array<float, 16>
      memset(constants.projection.data(), 0, sizeof(constants.projection)); // Using sizeof the object itself
      // constants_eye_projection is float4[2][4]
      memset(constants_eye_projection, 0, sizeof(constants_eye_projection));
      geometry_shader_manager.constants.stereoparams = { 0.0f, 0.0f, 0.0f, 0.0f };
    } else if (g_viewport_type == ViewportType::VIEW_RENDER_TO_TEXTURE) {
      Common::Matrix44 correctedMtx = Common::Matrix44::FromArray(g_fProjectionMatrix);
      // constants.projection is std::array<float, 16>
      memcpy(constants.projection.data(), correctedMtx.data.data(), sizeof(float) * 16);
      // constants_eye_projection is float4[2][4], so constants_eye_projection[0] is float4[4]
      memcpy(constants_eye_projection[0], correctedMtx.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[1], correctedMtx.data.data(), sizeof(float4) * 4);
      geometry_shader_manager.constants.stereoparams = { 0.0f, 0.0f, 0.0f, 0.0f };
    } else if (!g_has_hmd || !g_ActiveConfig.bEnableVR) {
      auto corrected_matrix = LoadProjectionMatrix();
      GraphicsModActionData::Projection projection_mod_data{&corrected_matrix};
      for (const auto& action : projection_actions_vec) action->OnProjection(&projection_mod_data);
      // constants.projection is std::array<float, 16>
      memcpy(constants.projection.data(), corrected_matrix.data.data(), sizeof(float) * 16);
      // constants_eye_projection is float4[2][4]
      memcpy(constants_eye_projection[0], corrected_matrix.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[1], corrected_matrix.data.data(), sizeof(float4) * 4);
      if (g_ActiveConfig.stereo_mode != StereoMode::Off) {
        if (xfmem.projection.type == ProjectionType::Perspective) {
          float offset = (g_ActiveConfig.iStereoDepth/1000.0f)*(g_ActiveConfig.iStereoDepthPercentage / 100.0f);
          Core::System::GetInstance().GetGeometryShaderManager().constants.stereoparams[0] = (g_ActiveConfig.bStereoSwapEyes)?offset:-offset;
          geometry_shader_manager.constants.stereoparams[1] = (g_ActiveConfig.bStereoSwapEyes)?-offset:offset;
        } else {
          geometry_shader_manager.constants.stereoparams[0]=0.0f; geometry_shader_manager.constants.stereoparams[1]=0.0f;
        }
        geometry_shader_manager.constants.stereoparams[2] = static_cast<float>(g_ActiveConfig.iStereoConvergence*(g_ActiveConfig.iStereoConvergencePercentage/100.0f));
        geometry_shader_manager.constants.stereoparams[3]=0.0f;
      } else { geometry_shader_manager.constants.stereoparams = { 0.0f, 0.0f, 0.0f, 0.0f }; }
    } else if (bFullscreenLayer) {
      Common::Matrix44 projMtx = Common::Matrix44::FromArray(g_fProjectionMatrix);
      projMtx.data[0] *= fWidthHack; projMtx.data[5] *= fHeightHack;
      projMtx.data[12] += fRightHack; projMtx.data[13] += fUpHack;
      memcpy(constants.projection.data(), projMtx.data.data(), sizeof(float) * 16);
      // constants_eye_projection is float4[2][4]
      memcpy(constants_eye_projection[0], projMtx.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[1], projMtx.data.data(), sizeof(float4) * 4);
      geometry_shader_manager.constants.stereoparams = { 0.0f, 0.0f, 0.0f, 0.0f };
    } else { // Main VR 3D HMD rendering path
      Common::Matrix44 final_matrix_left, final_matrix_right;
      CalculateVRProjectionViewMatrices(
            rawProjection.data(), xfmem.projection.type, UnitsPerMetre,
            bStuckToHead, bHideLeft, bHideRight, bNoForward, iTelescopeHack,
            fLeftWidthHack, fLeftHeightHack, fRightWidthHack, fRightHeightHack, fRightHack, fUpHack, bN64,
            final_matrix_left, final_matrix_right,
            geometry_shader_manager.constants.stereoparams);
      // constants.projection is std::array<float, 16>
      memcpy(constants.projection.data(), final_matrix_left.data.data(), sizeof(float) * 16);
      // constants_eye_projection is float4[2][4]
      memcpy(constants_eye_projection[0], final_matrix_left.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[1], final_matrix_right.data.data(), sizeof(float4) * 4);
    }

    if (!projection_actions_vec.empty()) {
        // constants.projection is now std::array<float, 16>, so FromArray works.
        Common::Matrix44 matrix_to_mod = Common::Matrix44::FromArray(constants.projection);
        GraphicsModActionData::Projection projection_mod_data{&matrix_to_mod};
        for (const auto& action : projection_actions_vec) action->OnProjection(&projection_mod_data);
        // constants.projection is std::array<float, 16>
        memcpy(constants.projection.data(), matrix_to_mod.data.data(), sizeof(float) * 16);
        if (g_has_hmd && g_ActiveConfig.bEnableVR) {
            // constants_eye_projection is float4[2][4]
             memcpy(constants_eye_projection[0], matrix_to_mod.data.data(), sizeof(float4) * 4);
        }
    }
    VertexShaderManager::dirty = false;
  }

  if (constants.missing_color_hex != g_ActiveConfig.iMissingColorValue) {
    u32 val = g_ActiveConfig.iMissingColorValue;
    constants.missing_color_hex = val;
    constants.missing_color_value = {((val>>24)&0xFF)/255.f, ((val>>16)&0xFF)/255.f, ((val>>8)&0xFF)/255.f, (val&0xFF)/255.f};
    dirty = true;
  }

  const auto per_vertex_transform_matrix_changes = xf_state_manager.GetPerVertexTransformMatrixChanges();
  if (per_vertex_transform_matrix_changes[0] >= 0) {
    int startn = per_vertex_transform_matrix_changes[0]/4; int endn = (per_vertex_transform_matrix_changes[1]+3)/4;
    memcpy(constants.transformmatrices[startn].data(), &xfmem.posMatrices[startn*4], (endn-startn)*sizeof(float4));
    dirty = true; xf_state_manager.ResetPerVertexTransformMatrixChanges();
  }

  const auto per_vertex_normal_matrices_changed = xf_state_manager.GetPerVertexNormalMatrixChanges();
  if (per_vertex_normal_matrices_changed[0] >= 0) {
    int startn = per_vertex_normal_matrices_changed[0]/3; int endn = (per_vertex_normal_matrices_changed[1]+2)/3;
    for(int i=startn;i<endn;i++) memcpy(constants.normalmatrices[i].data(),&xfmem.normalMatrices[3*i],12);
    dirty = true; xf_state_manager.ResetPerVertexNormalMatrixChanges();
  }

  const auto post_transform_matrices_changed = xf_state_manager.GetPostTransformMatrixChanges();
  if (post_transform_matrices_changed[0] >= 0) {
    int startn = post_transform_matrices_changed[0]/4; int endn = (post_transform_matrices_changed[1]+3)/4;
    memcpy(constants.posttransformmatrices[startn].data(),&xfmem.postMatrices[startn*4],(endn-startn)*sizeof(float4));
    dirty = true; xf_state_manager.ResetPostTransformMatrixChanges();
  }

  const auto light_changes = xf_state_manager.GetLightsChanged();
  if (light_changes[0] >= 0) {
    const int istart=light_changes[0]/0x10; const int iend=(light_changes[1]+15)/0x10;
    for (int i=istart; i<iend; ++i) {
      const Light& light = xfmem.lights[i]; VertexShaderConstants::Light& dstlight = constants.lights[i];
      dstlight.color[0]=light.color[3]; dstlight.color[1]=light.color[2]; dstlight.color[2]=light.color[1]; dstlight.color[3]=light.color[0];
      dstlight.cosatt[0]=light.cosatt[0]; dstlight.cosatt[1]=light.cosatt[1]; dstlight.cosatt[2]=light.cosatt[2];
      if(fabs(light.distatt[0])<0.00001f && fabs(light.distatt[1])<0.00001f && fabs(light.distatt[2])<0.00001f) {
        dstlight.distatt[0]=.00001f;
      } else { dstlight.distatt[0]=light.distatt[0]; }
      dstlight.distatt[1]=light.distatt[1]; dstlight.distatt[2]=light.distatt[2];
      dstlight.pos[0]=light.dpos[0]; dstlight.pos[1]=light.dpos[1]; dstlight.pos[2]=light.dpos[2];
      auto sanitize = [](float f) { return std::isnan(f)?0.0f:(std::isinf(f)?(f>0.0f?1.0f:-1.0f):f); };
      double norm = sqrt(double(light.ddir[0])*double(light.ddir[0]) + double(light.ddir[1])*double(light.ddir[1]) + double(light.ddir[2])*double(light.ddir[2]));
      norm = (norm == 0.0) ? 1.0 : 1.0 / norm; // Avoid division by zero, default to non-normalized if norm is zero
      dstlight.dir[0]=sanitize(static_cast<float>(light.ddir[0]*norm));
      dstlight.dir[1]=sanitize(static_cast<float>(light.ddir[1]*norm));
      dstlight.dir[2]=sanitize(static_cast<float>(light.ddir[2]*norm));
    }
    dirty = true; xf_state_manager.ResetLightsChanged();
  }

  for (int i : xf_state_manager.GetMaterialChanges()) {
    u32 data = i>=2 ? xfmem.matColor[i-2] : xfmem.ambColor[i];
    constants.materials[i][0]=(data>>24)&0xFF; constants.materials[i][1]=(data>>16)&0xFF;
    constants.materials[i][2]=(data>>8)&0xFF; constants.materials[i][3]=data&0xFF;
    dirty = true;
  }
  xf_state_manager.ResetMaterialChanges();

  if (xf_state_manager.DidPosNormalChange()) {
    xf_state_manager.ResetPosNormalChange();
    const float* pos = &xfmem.posMatrices[g_main_cp_state.matrix_index_a.PosNormalMtxIdx*4];
    const float* norm_ptr = &xfmem.normalMatrices[3*(g_main_cp_state.matrix_index_a.PosNormalMtxIdx&31)];
    memcpy(constants.posnormalmatrix.data(),pos,3*sizeof(float4));
    memcpy(constants.posnormalmatrix[3].data(),norm_ptr,3*sizeof(float));
    memcpy(constants.posnormalmatrix[4].data(),norm_ptr+3,3*sizeof(float));
    memcpy(constants.posnormalmatrix[5].data(),norm_ptr+6,3*sizeof(float));
    dirty = true;
  }

  if (xf_state_manager.DidTexMatrixAChange()) {
    xf_state_manager.ResetTexMatrixAChange();
    const std::array<const float*,4>ptrs{&xfmem.posMatrices[g_main_cp_state.matrix_index_a.Tex0MtxIdx*4],&xfmem.posMatrices[g_main_cp_state.matrix_index_a.Tex1MtxIdx*4],&xfmem.posMatrices[g_main_cp_state.matrix_index_a.Tex2MtxIdx*4],&xfmem.posMatrices[g_main_cp_state.matrix_index_a.Tex3MtxIdx*4]};
    for(size_t i=0;i<ptrs.size();++i)memcpy(constants.texmatrices[3*i].data(),ptrs[i],3*sizeof(float4));
    dirty = true;
  }

  if (xf_state_manager.DidTexMatrixBChange()) {
    xf_state_manager.ResetTexMatrixBChange();
    const std::array<const float*,4>ptrs{&xfmem.posMatrices[g_main_cp_state.matrix_index_b.Tex4MtxIdx*4],&xfmem.posMatrices[g_main_cp_state.matrix_index_b.Tex5MtxIdx*4],&xfmem.posMatrices[g_main_cp_state.matrix_index_b.Tex6MtxIdx*4],&xfmem.posMatrices[g_main_cp_state.matrix_index_b.Tex7MtxIdx*4]};
    for(size_t i=0;i<ptrs.size();++i)memcpy(constants.texmatrices[3*i+12].data(),ptrs[i],3*sizeof(float4));
    dirty = true;
  }

  if (xf_state_manager.DidTexMatrixInfoChange()) {
    xf_state_manager.ResetTexMatrixInfoChange();
    constants.xfmem_dualTexInfo = xfmem.dualTexTrans.enabled;
    for(size_t i=0;i<std::size(xfmem.texMtxInfo);i++)constants.xfmem_pack1[i][0]=xfmem.texMtxInfo[i].hex;
    for(size_t i=0;i<std::size(xfmem.postMtxInfo);i++)constants.xfmem_pack1[i][1]=xfmem.postMtxInfo[i].hex;
    dirty = true;
  }

  if (xf_state_manager.DidLightingConfigChange()) {
    xf_state_manager.ResetLightingConfigChange();
    for(size_t i=0;i<2;i++){constants.xfmem_pack1[i][2]=xfmem.color[i].hex;constants.xfmem_pack1[i][3]=xfmem.alpha[i].hex;}
    constants.xfmem_numColorChans = xfmem.numChan.numColorChans;
    dirty = true;
  }
}

void VertexShaderManager::TransformToClipSpace(const float* data, float* out, u32 MtxIdx)
{
  const float* world_matrix = &xfmem.posMatrices[(MtxIdx & 0x3f) * 4];
  const float* proj_matrix;
  if (g_has_hmd && g_ActiveConfig.bEnableVR) {
    proj_matrix = constants.projection.data();
  } else {
    proj_matrix = &m_projection_matrix[0];
  }
  const float t[3] = {data[0]*world_matrix[0]+data[1]*world_matrix[1]+data[2]*world_matrix[2]+world_matrix[3],
                      data[0]*world_matrix[4]+data[1]*world_matrix[5]+data[2]*world_matrix[6]+world_matrix[7],
                      data[0]*world_matrix[8]+data[1]*world_matrix[9]+data[2]*world_matrix[10]+world_matrix[11]};
  out[0] = t[0]*proj_matrix[0]+t[1]*proj_matrix[4]+t[2]*proj_matrix[8]+proj_matrix[12];
  out[1] = t[0]*proj_matrix[1]+t[1]*proj_matrix[5]+t[2]*proj_matrix[9]+proj_matrix[13];
  out[2] = t[0]*proj_matrix[2]+t[1]*proj_matrix[6]+t[2]*proj_matrix[10]+proj_matrix[14];
  out[3] = t[0]*proj_matrix[3]+t[1]*proj_matrix[7]+t[2]*proj_matrix[11]+proj_matrix[15];
}

void VertexShaderManager::DoState(PointerWrap& p)
{
  p.DoArray(m_projection_matrix);
  g_freelook_camera.DoState(p);
  p.Do(constants);

  p.Do(g_fProjectionMatrix);
  p.Do(s_locked_skybox_orientation);
  p.Do(s_had_skybox);
  // TODO: Handle enum serialization properly for g_viewport_type, g_old_viewport_type if needed
  p.Do(g_is_skybox);
  p.Do(g_game_camera_rotmat);
  p.Do(g_game_camera_pos);
  p.Do(g_vr_had_3D_already);
  p.Do(vr_widest_3d_HFOV);
  p.Do(vr_widest_3d_VFOV);
  p.Do(vr_widest_3d_zNear);
  p.Do(vr_widest_3d_zFar);

  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 4; ++j) {
      p.Do(constants_eye_projection[i][j]);
    }
  }
  p.Do(m_layer_on_top);

  if (p.IsReadMode()) {
    dirty = true;
  }
}
