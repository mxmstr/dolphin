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
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

void VertexShaderManager::Init()
{
  // Initialize state tracking variables from current
  m_projection_graphics_mod_change = false;
  constants = {}; // This zeros out VertexShaderConstants
  m_projection_matrix = Common::Matrix44::Identity().data; // Current projection sent to shader (will be one of eye projections)

  // Initialize new members based on Hydra's Init
  m_layer_on_top = false;
  m_viewport_type = VIEW_FULLSCREEN;
  m_old_viewport_type = VIEW_FULLSCREEN;
  m_is_skybox = false;

  ResetView(); // Initializes m_viewTranslationVector, m_viewRotationMatrix, m_viewInvRotationMatrix, m_viewRotation

  m_viewportCorrectionMatrix = Common::Matrix44::Identity();
  m_vrTotalMatrix = Common::Matrix44::Identity(); // Or specific initial projection

  // Initialize game's original projection matrix (g_fProjectionMatrix in Hydra)
  m_gameProjectionMatrix = Common::Matrix44::Identity(); // Default to identity, loaded in SetConstants

  m_stabilizedGameCameraRot = Common::Matrix44::Identity();
  std::memset(m_stabilizedGameCameraPos, 0, sizeof(m_stabilizedGameCameraPos));

  m_had_skybox_locked = false;
  std::memset(m_locked_skybox_matrix, 0, sizeof(m_locked_skybox_matrix));

  // Eye projections (can be identity or specific default)
  for (int i = 0; i < 4; ++i) {
    eye_projection_left[i] = float4(0.f,0.f,0.f,0.f);
    eye_projection_right[i] = float4(0.f,0.f,0.f,0.f);
    if (i==0) eye_projection_left[i].x = eye_projection_right[i].x = 1.f;
    if (i==1) eye_projection_left[i].y = eye_projection_right[i].y = 1.f;
    if (i==2) eye_projection_left[i].z = eye_projection_right[i].z = 1.f;
    if (i==3) eye_projection_left[i].w = eye_projection_right[i].w = 1.f;
  }


  // TODO: Initialize change tracking bools if they become members (e.g., m_bProjectionChanged = true)
  // For now, relying on XFStateManager and overall 'dirty' flag.

  dirty = true;
}

// Implementation for ResetView (moved from static in Hydra to member)
void VertexShaderManager::ResetView()
{
  // VRTracker::ResetView(); // If VRTracker is a class providing this. Assuming it's not integrated yet.

  std::memset(m_viewTranslationVector, 0, sizeof(m_viewTranslationVector));
  m_viewRotationMatrix.Identity();
  m_viewInvRotationMatrix.Identity();
  m_viewRotation[0] = 0.0f; // Yaw
  m_viewRotation[1] = 0.0f; // Pitch

  // bFreeLookChanged = false; // If this becomes a member
  SetProjectionChanged(); // Signal that projection needs recalculation
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
    memcpy(constants.projection.data(), corrected_matrix.data.data(), 4 * sizeof(float4));
  }
}

// --- VR Method Implementations (Adapted from Hydra) ---

void VertexShaderManager::SetViewportChanged()
{
  this->dirty = true;
}

void VertexShaderManager::SetProjectionChanged()
{
  this->dirty = true;
}

void VertexShaderManager::TranslateView(float left_metres, float forward_metres, float down_metres)
{
  float vector[3] = {left_metres, down_metres, forward_metres};
  // TODO: Apply world scale based on g_ActiveConfig.fScale and unitsPerMetre from VR settings.
  // Current m_viewInvRotationMatrix is assumed to be based on current free-look orientation.
  float result[3];
  m_viewInvRotationMatrix.Multiply(vector, result);

  for (size_t i = 0; i < 3; i++)
    m_viewTranslationVector[i] += result[i];

  SetProjectionChanged(); // Mark that view/projection matrices need update
}

void VertexShaderManager::RotateView(float x_rad, float y_rad) // x_rad is pitch, y_rad is yaw
{
  m_viewRotation[0] += y_rad; // Yaw component
  m_viewRotation[1] += x_rad; // Pitch component

  // Clamp pitch to avoid flipping over
  constexpr float half_pi = static_cast<float>(M_PI_2);
  m_viewRotation[1] = std::max(-half_pi + 0.001f, std::min(m_viewRotation[1], half_pi - 0.001f));

  Common::Matrix33 matrix_yaw, matrix_pitch;
  matrix_yaw.RotateY(m_viewRotation[0]);
  matrix_pitch.RotateX(m_viewRotation[1]);

  m_viewRotationMatrix = matrix_yaw * matrix_pitch; // Common convention: Yaw then Pitch

  m_viewInvRotationMatrix = m_viewRotationMatrix;
  if (!m_viewInvRotationMatrix.Invert()) // In-place invert and check success
  {
    m_viewInvRotationMatrix.Identity(); // Reset if inversion failed
    ERROR_LOG_FMT(VIDEO, "View matrix inversion failed in RotateView");
  }

  SetProjectionChanged();
}

void VertexShaderManager::ScaleView(float scale)
{
  // This was used in Hydra to scale the view translation vector when world scale changed.
  for (int i = 0; i < 3; i++)
    m_viewTranslationVector[i] *= scale;
  SetProjectionChanged();
}

// ResetView is already defined as part of Init and called from there if this file is kept structured this way.

void VertexShaderManager::ClassifyCurrentDrawCall(XFStateManager& xf_state_manager)
{
  // Adapted from SetViewportType in Hydra's VertexShaderManager.cpp
  m_old_viewport_type = m_viewport_type;

  const Viewport& vp = xfmem.viewport;
  // Viewport coordinates in Hydra were relative to a 342-offset origin.
  // Here, vp.xOrig and vp.yOrig are likely direct EFB coordinates.
  // vp.wd and vp.ht are half-width and half-height. ht is often negative.
  float left = vp.xOrig - vp.wd; // Effective left edge
  float top = vp.yOrig + vp.ht;  // Effective top edge (since ht is negative, this is yOrigin - abs(ht))
  float width = 2.0f * vp.wd;
  float height = -2.0f * vp.ht; // Actual height

  // Use EFB dimensions from FramebufferManager for target screen size
  float screen_width = static_cast<float>(g_framebuffer_manager->GetEFBWidth());
  float screen_height = static_cast<float>(g_framebuffer_manager->GetEFBHeight());

  // Tolerances for floating point comparisons
  const float size_tolerance = 2.0f; // Pixel tolerance for width/height
  const float pos_tolerance = 2.0f; // Pixel tolerance for position

  // Heuristics from Hydra, adapted:
  float min_screen_width = 0.90f * screen_width;
  float min_screen_height = 0.90f * screen_height;
  float max_top_offset = screen_height - min_screen_height; // Max allowable offset from top for fullscreen-like
  float max_left_offset = screen_width - min_screen_width; // Max allowable offset from left

  // Default assumption
  m_viewport_type = VIEW_HUD_ELEMENT;


  // Power of two square viewport in the corner of the screen means we are rendering to a texture.
  // Hydra relaxed this to: square texture on any screen edge with size a multiple of 8.
  // Twilight Princess GC uses 216x216, 384x384. Metroid Prime 2 uses square textures.
  // Relaxed rule: square texture on any screen edge, size multiple of 8, or known game-specific sizes.
  // Skip if it's full EFB size (e.g. Bad Boys 2 512x512 viewport on 512x512 EFB).
  bool is_square = std::abs(width - height) < size_tolerance;
  bool is_power_of_two_or_special_size = false;
  if (is_square) {
    int int_width = static_cast<int>(width + 0.5f);
    if (int_width == 1 || int_width == 2 || int_width == 4 || (int_width % 8 == 0) ||
        int_width == 216 || int_width == 384 || int_width == 457) // 457x341 for TP map
    {
        is_power_of_two_or_special_size = true;
    }
  }

  bool on_edge = (std::abs(left) < pos_tolerance || std::abs(top) < pos_tolerance ||
                  std::abs(top + height - screen_height) < pos_tolerance ||
                  std::abs(left + width - screen_width) < pos_tolerance);
  bool is_full_efb_match = (std::abs(width - screen_width) < size_tolerance && std::abs(height - screen_height) < size_tolerance);


  if (is_square && is_power_of_two_or_special_size && on_edge && !is_full_efb_match) {
    m_viewport_type = VIEW_RENDER_TO_TEXTURE;
  }
  // Zelda TP specific render-to-texture for map highlights (457x341)
  else if (std::abs(width - 457.f) < size_tolerance && std::abs(height - 341.f) < size_tolerance &&
           std::abs(left) < pos_tolerance && std::abs(top) < pos_tolerance)
  {
    m_viewport_type = VIEW_RENDER_TO_TEXTURE;
  }
  // Full width could mean fullscreen, letterboxed, or splitscreen top/bottom.
  else if (width >= min_screen_width && std::abs(left) <= max_left_offset)
  {
    if (height >= min_screen_height && std::abs(top) <= max_top_offset)
    {
        // Check if it's *exactly* fullscreen or just very close (letterboxed/pillarboxed slightly)
        if (std::abs(width - screen_width) < size_tolerance && std::abs(height - screen_height) < size_tolerance &&
            std::abs(left) < pos_tolerance && std::abs(top) < pos_tolerance)
        {
            m_viewport_type = VIEW_FULLSCREEN;
        } else {
            m_viewport_type = VIEW_LETTERBOXED; // Covers pillarbox too if width < screen_width
        }
    }
    // Split screen top/bottom checks (simplified)
    else if (height >= min_screen_height / 2.0f - size_tolerance && height <= screen_height / 2.0f + size_tolerance)
    {
        if (std::abs(top) < max_top_offset) { // Top half
            m_viewport_type = VIEW_PLAYER_1;
            // TODO: Set a global splitscreen_type if that concept is ported
        } else if (std::abs(top - (screen_height / 2.0f)) < max_top_offset) { // Bottom half
            m_viewport_type = VIEW_PLAYER_2;
        } else {
            m_viewport_type = VIEW_LETTERBOXED; // Could be a band in the middle
        }
    } else {
        m_viewport_type = VIEW_LETTERBOXED; // Default for full-width but not full-height
    }
  }
  // Full height could mean splitscreen left/right.
  else if (height >= min_screen_height && std::abs(top) <= max_top_offset)
  {
    if (width >= min_screen_width / 2.0f - size_tolerance && width <= screen_width / 2.0f + size_tolerance)
    {
        if (std::abs(left) < max_left_offset) { // Left half
            m_viewport_type = VIEW_PLAYER_1;
        } else if (std::abs(left - (screen_width / 2.0f)) < max_left_offset) { // Right half
            m_viewport_type = VIEW_PLAYER_2;
        } else {
            m_viewport_type = VIEW_HUD_ELEMENT; // Column in the middle
        }
    } else {
        m_viewport_type = VIEW_LETTERBOXED; // Default for full-height but not full-width (pillarbox)
    }
  }
  // Quadrants (simplified)
  else if (width >= (min_screen_width / 2.f) - size_tolerance && height >= (min_screen_height / 2.f) - size_tolerance &&
           width <= (screen_width / 2.f) + size_tolerance && height <= (screen_height / 2.f) + size_tolerance)
  {
    if (std::abs(left) < max_left_offset && std::abs(top) < max_top_offset) m_viewport_type = VIEW_PLAYER_1; // TL
    else if (std::abs(left - screen_width/2.f) < max_left_offset && std::abs(top) < max_top_offset) m_viewport_type = VIEW_PLAYER_2; // TR
    else if (std::abs(left) < max_left_offset && std::abs(top - screen_height/2.f) < max_top_offset) m_viewport_type = VIEW_PLAYER_3; // BL
    else if (std::abs(left - screen_width/2.f) < max_left_offset && std::abs(top - screen_height/2.f) < max_top_offset) m_viewport_type = VIEW_PLAYER_4; // BR
    else m_viewport_type = VIEW_HUD_ELEMENT;
  }
  // Offscreen check (coordinates are way outside EFB)
  else if (left >= screen_width || top >= screen_height || (left + width) <= 0 || (top + height) <= 0)
  {
    m_viewport_type = VIEW_OFFSCREEN;
  }
  // Default to HUD if no other classification fits
  else
  {
    m_viewport_type = VIEW_HUD_ELEMENT;
  }

  bool old_skybox_status = m_is_skybox;
  m_is_skybox = false; // Reset before check

  if (g_ActiveConfig.bDetectSkybox &&
      (m_viewport_type == VIEW_FULLSCREEN || m_viewport_type == VIEW_LETTERBOXED ||
       (m_viewport_type >= VIEW_PLAYER_1 && m_viewport_type <= VIEW_PLAYER_4)))
  {
    // In Hydra, skybox was also checked if znear >= 0.99f && zfar >= 0.999f within SetViewportType.
    // This check is now primarily in CheckSkybox based on matrix properties.
    // We might add a preliminary z-depth check here if needed.
    // For now, CheckSkybox() called from SetConstants will handle it.
    // If CheckSkybox() is called here, ensure it doesn't cause recursive issues or depend on uninitialized state.
    // For now, deferring CheckSkybox() to SetConstants as it needs modelview matrix.
  }
  // If CheckSkybox (called later by SetConstants) sets m_is_skybox, it will override.
  // The skybox flag is finalized in SetConstants after CheckSkybox() has run.

  if (m_old_viewport_type != m_viewport_type || old_skybox_status != m_is_skybox) {
    this->dirty = true; // Signal change if viewport type or skybox status changed
  }
}

bool VertexShaderManager::IsConsideredHUD(ViewportType type) const
{
  // From Hydra:
  // A HUD element can be a specific HUD_ELEMENT type, or a letterboxed view,
  // or if bHudFullscreen is true, even player views can be treated as HUD for projection.
  return type == VIEW_HUD_ELEMENT ||
         type == VIEW_LETTERBOXED ||
        (type >= VIEW_PLAYER_1 && type <= VIEW_PLAYER_4 && g_ActiveConfig.bHudFullscreen);
}

void VertexShaderManager::ApplySkyboxTransformations()
{
  // This function will be called from SetConstants when m_is_skybox is true.
  // It needs to set constants.projection and constants.stereoparams
  // based on VR HMD orientation, game projection, and skybox-specific rules.

  // Skybox is typically independent of player position, only rotation.
  // It uses the game's projection matrix as a base.
  Common::Matrix44 vr_view_matrix = Common::Matrix44::Identity();

  // Apply HMD orientation (rotation only for skybox)
  // This would typically come from VR::GetHead orientación matrix.
  // Common::Matrix44 hmd_orientation = VR::GetHeadOrientationMatrix(); // Placeholder
  // vr_view_matrix *= hmd_orientation;


  if (g_ActiveConfig.iMotionSicknessSkybox == 2 && m_had_skybox_locked)
  {
    // Use locked skybox matrix (modelview part)
    // The m_locked_skybox_matrix is a 3x4 matrix. Convert to 4x4.
    Common::Matrix44 locked_mv;
    locked_mv.mData[0][0] = m_locked_skybox_matrix[0]; locked_mv.mData[0][1] = m_locked_skybox_matrix[1]; locked_mv.mData[0][2] = m_locked_skybox_matrix[2]; locked_mv.mData[0][3] = m_locked_skybox_matrix[3];
    locked_mv.mData[1][0] = m_locked_skybox_matrix[4]; locked_mv.mData[1][1] = m_locked_skybox_matrix[5]; locked_mv.mData[1][2] = m_locked_skybox_matrix[6]; locked_mv.mData[1][3] = m_locked_skybox_matrix[7];
    locked_mv.mData[2][0] = m_locked_skybox_matrix[8]; locked_mv.mData[2][1] = m_locked_skybox_matrix[9]; locked_mv.mData[2][2] = m_locked_skybox_matrix[10];locked_mv.mData[2][3] = m_locked_skybox_matrix[11];
    locked_mv.mData[3][0] = 0;                       locked_mv.mData[3][1] = 0;                       locked_mv.mData[3][2] = 0;                        locked_mv.mData[3][3] = 1;
    vr_view_matrix = locked_mv * vr_view_matrix; // Apply HMD orientation to the locked skybox view
  }
  else
  {
    // If not locked or no lock data, use current game modelview (which for skybox should be mostly rotation)
    // and apply HMD orientation to it.
    // vr_view_matrix = GetCurrentGameModelViewMatrix() * vr_view_matrix; // This might be too much if GetCurrent also has world translation
    // For skybox, we usually want to remove game camera translation.
    // HMD rotation is usually sufficient.
  }


  // Calculate stereo projections using the modified view and game's base projection
  CalculateStereoProjectionsAndViewports(m_gameProjectionMatrix, vr_view_matrix);

  // Update GS stereo parameters
  UpdateStereoParamsForGS();

  // Set constants.projection to the left eye's projection for shader use
  // The GS will handle selecting left/right view based on stereoparams and instance ID
  memcpy(constants.projection.data(), m_eyeProjectionLeft.GetData(), 4 * sizeof(float4));

  this->dirty = true;
}

void VertexShaderManager::ApplyHUDTransformations()
{
  // HUD elements are typically drawn with an orthographic projection or a fixed perspective.
  // They need to be placed in 3D space relative to the HMD.

  float UnitsPerMetre = g_ActiveConfig.fUnitsPerMetre * g_ActiveConfig.fScale; // Use configured scale

  // Base HUD projection: often orthographic, or a specific perspective for "3D" HUD elements.
  // For simplicity, let's assume game's projection is used if it's ortho, or a default ortho if perspective.
  Common::Matrix44 hud_base_projection = m_gameProjectionMatrix;
  if (xfmem.projection.type == ProjectionType::Perspective) {
      // Create a default orthographic projection for HUD if game uses perspective for it
      // This needs to match how the game expects its HUD to be rendered.
      // Or, use a fixed perspective that makes sense for a "pane of glass" HUD.
      // For now, we'll just use the game's projection, assuming it might be ortho for HUD.
      // A more robust solution would create a canonical ortho projection based on EFB dimensions.
  }


  // HUD view matrix: positions the HUD plane in front of the HMD.
  Common::Matrix44 hud_view_matrix = Common::Matrix44::Identity();

  // 1. Scale: Determine the size of the HUD plane in world units.
  //    This depends on desired perceived size and distance.
  //    Hydra calculates HudWidth/HudHeight based on fov and distance.
  float hud_distance_meters = g_ActiveConfig.fHudDistance;
  float hud_depth_meters = g_ActiveConfig.fHudThickness; // How "thick" the HUD appears

  // Convert game's screen space coordinates (often -1 to 1 in clip space for ortho) to world units.
  // This requires knowing the original screen dimensions the HUD was designed for.
  // For now, let's assume the m_gameProjectionMatrix handles the base scaling to clip space.
  // We then transform this clip space representation into a 3D plane.

  // Position the HUD plane at hud_distance_meters in front of the HMD.
  // Common::Matrix44 translation_to_hud_plane;
  // translation_to_hud_plane.Translate(0, 0, -hud_distance_meters * UnitsPerMetre); // Z is negative into the screen

  // HMD orientation (don't apply HMD position for HUD, it's head-locked)
  // Common::Matrix44 hmd_orientation = VR::GetHeadOrientationMatrix(); // Placeholder

  // hud_view_matrix = hmd_orientation * translation_to_hud_plane;

  // More complete HUD transformation from Hydra:
  // It involves creating a scale and position matrix for the HUD quad based on its original 2D/ortho projection
  // and then placing that quad in 3D space relative to the HMD.
  // This is quite complex and involves assumptions about original HUD rendering.

  // Simplified: Assume m_gameProjectionMatrix is what the HUD uses.
  // We want to make this projection appear on a plane at a fixed distance, scaled appropriately.
  // This is where a specific "HUD camera" would be constructed.
  // For now, we'll use a very basic approach: apply HMD rotation and a fixed translation.

  Common::Matrix44 hmd_transform = Common::Matrix44::Identity(); // This would be VR::GetHeadTransform()
  // For HUD, usually only rotation part of HMD transform is used, plus a fixed offset.
  // hmd_transform.SetTranslation(Common::Vec3(0,0,0)); // Remove HMD positional tracking for basic head-locked HUD

  Common::Matrix44 offset_transform;
  offset_transform.Translate(0, 0, -hud_distance_meters * UnitsPerMetre); // Place HUD in front

  hud_view_matrix = hmd_transform * offset_transform;


  // Calculate stereo projections for the HUD
  CalculateStereoProjectionsAndViewports(hud_base_projection, hud_view_matrix);
  UpdateStereoParamsForGS();
  memcpy(constants.projection.data(), m_eyeProjectionLeft.GetData(), 4 * sizeof(float4));

  this->dirty = true;
}

void VertexShaderManager::ApplyWorldTransformations()
{
  // This is for the main 3D world rendering.
  // It combines game camera, free look, HMD tracking, and stabilization.

  float UnitsPerMetre = g_ActiveConfig.fUnitsPerMetre * g_ActiveConfig.fScale;

  // 1. Start with game's modelview matrix (from XF state)
  //    However, we usually apply VR transformations to the *projection* side,
  //    and the game's modelview is applied by the fixed function pipeline or shader as usual.
  //    The "VR view matrix" combines all player-centric views (free look, HMD)
  //    and game camera stabilization.
  Common::Matrix44 vr_view_matrix = Common::Matrix44::Identity();


  // 2. Camera Position Stabilization (from CheckOrientationConstants)
  //    This needs g_game_camera_pos (world space position of stabilized game camera)
  //    and g_game_camera_rotmat (stabilized game camera rotation).
  //    This is complex. For now, assume these are updated elsewhere if stabilization is on.
  //    If g_ActiveConfig.bStabilizeX/Y/Z:
  //       Common::Matrix44 game_cam_stabilized_pos_inv;
  //       game_cam_stabilized_pos_inv.Translate(-m_stabilizedGameCameraPos[0], -m_stabilizedGameCameraPos[1], -m_stabilizedGameCameraPos[2]);
  //       vr_view_matrix *= game_cam_stabilized_pos_inv;
  //
  //    If g_ActiveConfig.bStabilizePitch/Yaw/Roll:
  //       vr_view_matrix *= m_stabilizedGameCameraRot.Inverse(); // Apply inverse of stabilized game rotation


  // 3. Free Look (m_viewRotationMatrix, m_viewTranslationVector)
  Common::Matrix44 free_look_translation_matrix;
  free_look_translation_matrix.Translate(-m_viewTranslationVector[0] * UnitsPerMetre,
                                       -m_viewTranslationVector[1] * UnitsPerMetre,
                                       -m_viewTranslationVector[2] * UnitsPerMetre);
  Common::Matrix44 free_look_rotation_matrix(m_viewRotationMatrix); // Assuming m_viewRotationMatrix is view (inverse of camera)

  vr_view_matrix *= free_look_translation_matrix;
  vr_view_matrix *= free_look_rotation_matrix;


  // 4. HMD Tracking (Position and Orientation)
  // Common::Matrix44 hmd_transform = VR::GetHeadTransform(); // Placeholder for actual HMD transform
  // vr_view_matrix *= hmd_transform.Inverse(); // Apply inverse HMD transform to camera


  // The game's original projection matrix (m_gameProjectionMatrix) is the base.
  CalculateStereoProjectionsAndViewports(m_gameProjectionMatrix, vr_view_matrix);
  UpdateStereoParamsForGS();
  memcpy(constants.projection.data(), m_eyeProjectionLeft.GetData(), 4 * sizeof(float4));

  this->dirty = true;
}


void VertexShaderManager::CalculateStereoProjectionsAndViewports(const Common::Matrix44& base_projection, const Common::Matrix44& view_matrix_offset)
{
    // base_projection is the game's original projection (e.g., m_gameProjectionMatrix)
    // view_matrix_offset includes HMD tracking, free look, stabilization etc.

    // TODO: Get HMD projection matrices from VR system (e.g. OpenVR via VR::GetProjectionMatrices)
    // These are raw projection matrices from the HMD, typically for Z near/far of 0.1 to 1000 or similar.
    // We need to adapt them to the game's Z range if possible, or use the HMD's Z range and hope for the best.
    // For now, let's assume VR::GetProjectionMatrices gives us m_eyeProjectionLeft/Right directly.

    // Example: VR::GetProjectionMatrices(m_eyeProjectionLeft, m_eyeProjectionRight, game_znear, game_zfar);
    // where game_znear/zfar are extracted from base_projection.
    // If such an API exists, it would populate m_eyeProjectionLeft and m_eyeProjectionRight.

    // If VR system provides eye offsets (IPD) and we need to construct them:
    // float ipd_meters = VR::GetUserIPD(); // Placeholder
    // float eye_offset_world = (ipd_meters / 2.0f) * (g_ActiveConfig.fUnitsPerMetre * g_ActiveConfig.fScale);
    // Common::Matrix44 left_eye_offset_matrix, right_eye_offset_matrix;
    // left_eye_offset_matrix.Translate(-eye_offset_world, 0, 0);
    // right_eye_offset_matrix.Translate(eye_offset_world, 0, 0);

    // Final view for each eye: game_modelview * view_matrix_offset * eye_offset_HMD_space
    // Then projection: eye_projection_HMD * final_view_eye

    // Simplified: Assume VR system gives full projection matrices per eye (m_rawEyeProjectionL/R)
    // And provides eye-to-head transforms (m_rawEyeOffsetL/R)
    // Common::Matrix44 hmd_proj_l, hmd_proj_r; // From VR_GetProjectionMatrices
    // Common::Matrix44 eye_to_head_l, eye_to_head_r; // From VR_GetEyeToHeadTransforms

    // m_eyeProjectionLeft  = hmd_proj_l * eye_to_head_l.Inverse() * view_matrix_offset.Inverse() * base_projection;
    // m_eyeProjectionRight = hmd_proj_r * eye_to_head_r.Inverse() * view_matrix_offset.Inverse() * base_projection;
    // This is complex. Let's assume a simpler path where HMD gives projections, and we combine with our view_matrix_offset.

    // For now, as a placeholder until proper VR SDK integration:
    // Use base_projection for both, and view_matrix_offset is applied to modelview implicitly by shaders,
    // or explicitly if we modify the 'constants.projection' to be proj * view_offset.
    // Let's assume constants.projection will be P_hmd * V_vr_offset * V_game_original_inverse.
    // And game shaders do V_game_original * M_model.
    // So we need to supply P_hmd * V_vr_offset to constants.projection.

    // If view_matrix_offset is to be applied before projection:
    m_eyeProjectionLeft = base_projection; // This would be HMD's left eye projection
    m_eyeProjectionRight = base_projection; // This would be HMD's right eye projection

    // And constants.projection would be: m_eyeProjectionLeft * view_matrix_offset
    // OR, if view_matrix_offset is part of the "camera" that the HMD projection acts upon:
    // The view_matrix_offset should effectively be part of the view matrix.
    // So, constants.projection = specific_eye_hmd_projection.
    // And the effective view matrix becomes: view_matrix_offset * game_modelview_matrix.
    // This means game_modelview_matrix must be accessible or passed around.

    // Let's follow Hydra's model more closely for SetProjectionConstants structure.
    // Hydra builds a final_matrix_left/right which is effectively an MVP matrix.
    // And constants.projection gets final_matrix_left.
    // This means view_matrix_offset is combined with eye offsets and then with the base_projection.

    // For now, this function will just store the HMD's projection matrices.
    // The view_matrix_offset will be combined in Apply*Transformations.
    // This function should ideally fetch raw HMD projections.
    // VR::GetRawProjectionMatrices(&m_eyeProjectionLeft, &m_eyeProjectionRight, znear, zfar_from_game_proj);
    // As a placeholder:
    if (g_ActiveConfig.bEnableVR) { // Only if VR is enabled
        // Simulate fetching HMD projections. These would be asymmetric.
        // Example: slightly offset projection matrix based on IPD.
        // This is a very rough approximation. Actual matrices come from OpenVR.
        float ipd_offset_clip_space = 0.02f; // Approximate horizontal shift in clip space for stereo
        m_eyeProjectionLeft = base_projection;
        m_eyeProjectionLeft.mData[0][3] -= ipd_offset_clip_space; // Shift left eye rendering left

        m_eyeProjectionRight = base_projection;
        m_eyeProjectionRight.mData[0][3] += ipd_offset_clip_space; // Shift right eye rendering right
    } else {
        m_eyeProjectionLeft = base_projection;
        m_eyeProjectionRight = base_projection;
    }


    // Store them in the float4 arrays if still needed by some path
    memcpy(eye_projection_left, m_eyeProjectionLeft.GetData(), 4 * sizeof(float4));
    memcpy(eye_projection_right, m_eyeProjectionRight.GetData(), 4 * sizeof(float4));
    this->dirty = true;
}


void VertexShaderManager::UpdateStereoParamsForGS()
{
    // This should derive values from m_eyeProjectionLeft/Right or HMD properties
    // These parameters are used by the Geometry Shader to instance rendering for each eye.
    // constants.stereoparams.x = Left Eye Projection matrix element [0][0] (or similar, depends on GS needs)
    // constants.stereoparams.y = Right Eye Projection matrix element [0][0]
    // constants.stereoparams.z = Left Eye Projection matrix element [0][2] (horizontal offset)
    // constants.stereoparams.w = Right Eye Projection matrix element [0][2] (horizontal offset)
    // This is highly dependent on what the Geometry Shader expects.
    // Hydra's SetProjectionConstants:
    // GeometryShaderManager::constants.stereoparams[0] = proj_left.data[0 * 4 + 0]; (xx)
    // GeometryShaderManager::constants.stereoparams[1] = proj_right.data[0 * 4 + 0]; (xx)
    // GeometryShaderManager::constants.stereoparams[2] = proj_left.data[0 * 4 + 2]; (wx, off-axis component)
    // GeometryShaderManager::constants.stereoparams[3] = proj_right.data[0 * 4 + 2]; (wx, off-axis component)

    if (g_ActiveConfig.bEnableVR) { // Or g_ActiveConfig.stereo_mode != StereoMode::Off
        constants.stereoparams.x = m_eyeProjectionLeft.mData[0][0];
        constants.stereoparams.y = m_eyeProjectionRight.mData[0][0];
        constants.stereoparams.z = m_eyeProjectionLeft.mData[0][2]; // This is projection_matrix[2] in math terms (M02)
        constants.stereoparams.w = m_eyeProjectionRight.mData[0][2];// This is projection_matrix[2] in math terms (M02)
    } else { // Non-VR stereo or mono
        // For anaglyph or side-by-side without GS instancing, these might be different.
        // If GS is used for simple stereo (non-VR), it might still need these.
        // If no stereo or GS isn't used for stereo, these can be zero.
        float offset = 0.0f;
        if (g_ActiveConfig.stereo_mode != StereoMode::Off) { // Non-VR stereo
             // Simplified: anaglyph might use a color shift or a slight position offset.
             // SBS/TAB are often handled by post-processing or viewport manipulation.
             // If GS is used for non-VR stereo, it might need IPD-like offsets.
            offset = g_ActiveConfig.fStereoDepth / 1000.0f; // Example value
        }
        constants.stereoparams.x = m_gameProjectionMatrix.mData[0][0]; // Base projection xx
        constants.stereoparams.y = m_gameProjectionMatrix.mData[0][0]; // Base projection xx
        constants.stereoparams.z = -offset; // Left eye offset (example)
        constants.stereoparams.w = offset;  // Right eye offset (example)
    }
    this->dirty = true;
}

void VertexShaderManager::CheckOrientationConstants()
{
  // Ported from Hydra's VertexShaderManager::CheckOrientationConstants
  // This function reads game camera orientation and position for stabilization.
  // It requires g_ActiveConfig.bCanReadCameraAngles and other stabilization flags.
  // It updates m_stabilizedGameCameraPos and m_stabilizedGameCameraRot.

  // This logic is highly game-specific and relies on heuristics (e.g., min polygon count)
  // and assumptions about where camera data is in xfmem (e.g., constants.posnormalmatrix).
  // For a generic port, we'll include the structure but acknowledge it might need
  // game-specific tuning or a more robust way to get "game camera" info.

  bool can_read = g_ActiveConfig.bCanReadCameraAngles &&
                  (g_ActiveConfig.bStabilizePitch || g_ActiveConfig.bStabilizeRoll ||
                   g_ActiveConfig.bStabilizeYaw || g_ActiveConfig.bStabilizeX ||
                   g_ActiveConfig.bStabilizeY || g_ActiveConfig.bStabilizeZ);

  // TODO: Add prim_count check from Hydra if Statistics collection is similar.
  // int prim_count = stats.prevFrame.numPrims + stats.prevFrame.numDLPrims;
  // if (prim_count < (int)g_ActiveConfig.iCameraMinPoly) can_read = false;

  if (can_read)
  {
    // constants.posnormalmatrix contains the current main modelview (pos) and normal matrix.
    // Hydra extracts game camera from the first "real 3D object drawn".
    // This assumes constants.posnormalmatrix is that object's matrix.
    const float* pnm_ptr = constants.posnormalmatrix[0].data(); // First row of 4x4 matrix
    Common::Matrix44 current_game_mv; // Assuming posnormalmatrix is effectively the ModelView matrix here
    current_game_mv.mData[0][0] = pnm_ptr[0]; current_game_mv.mData[0][1] = pnm_ptr[1]; current_game_mv.mData[0][2] = pnm_ptr[2]; current_game_mv.mData[0][3] = pnm_ptr[3];
    pnm_ptr = constants.posnormalmatrix[1].data();
    current_game_mv.mData[1][0] = pnm_ptr[0]; current_game_mv.mData[1][1] = pnm_ptr[1]; current_game_mv.mData[1][2] = pnm_ptr[2]; current_game_mv.mData[1][3] = pnm_ptr[3];
    pnm_ptr = constants.posnormalmatrix[2].data();
    current_game_mv.mData[2][0] = pnm_ptr[0]; current_game_mv.mData[2][1] = pnm_ptr[1]; current_game_mv.mData[2][2] = pnm_ptr[2]; current_game_mv.mData[2][3] = pnm_ptr[3];
    current_game_mv.mData[3][0] = 0; current_game_mv.mData[3][1] = 0; current_game_mv.mData[3][2] = 0; current_game_mv.mData[3][3] = 1;


    Common::Vec3 pos_camera_space = current_game_mv.GetTranslation();
    Common::Matrix33 rot_matrix_33 = current_game_mv.ToMatrix33();

    // Normalize rotation matrix (Hydra does this by checking scale factor)
    float scale_x = Common::Vec3(rot_matrix_33(0,0), rot_matrix_33(1,0), rot_matrix_33(2,0)).Length();
    if (std::abs(scale_x) > 0.001f && std::abs(scale_x - 1.0f) > 0.001f) { // If not identity scale
        rot_matrix_33(0,0) /= scale_x; rot_matrix_33(0,1) /= scale_x; rot_matrix_33(0,2) /= scale_x;
        rot_matrix_33(1,0) /= scale_x; rot_matrix_33(1,1) /= scale_x; rot_matrix_33(1,2) /= scale_x;
        rot_matrix_33(2,0) /= scale_x; rot_matrix_33(2,1) /= scale_x; rot_matrix_33(2,2) /= scale_x;
    }


    // Convert camera-space position to world space (undo rotation part of MV)
    Common::Matrix33 inv_rot_matrix = rot_matrix_33.Inverse(); // Assuming it's orthogonal, Transpose == Inverse
    Common::Vec3 world_space_pos = inv_rot_matrix * pos_camera_space;

    // TODO: Movement calculation and `totalpos` logic from Hydra if needed for more advanced stabilization.
    // For now, directly use current world_space_pos and rot_matrix_33.
    // Hydra's `totalpos` was an attempt to accumulate movement.

    m_stabilizedGameCameraPos[0] = world_space_pos.x;
    m_stabilizedGameCameraPos[1] = world_space_pos.y;
    m_stabilizedGameCameraPos[2] = world_space_pos.z;

    // Extract Yaw, Pitch, Roll from rot_matrix_33
    // Common::Vec3 ypr = rot_matrix_33.GetEulerAnglesZYX(); // Or similar Euler extraction
    // float game_yaw = ypr.x; float game_pitch = ypr.y; float game_roll = ypr.z;
    // This part is tricky and depends on Euler angle convention. Hydra uses Matrix33::GetPieYawPitchRollR.
    // For now, let's assume rot_matrix_33 is the game's camera orientation.

    Common::Matrix33 stabilized_rot_33;
    // Based on g_ActiveConfig.bStabilizePitch/Yaw/Roll, selectively build stabilized_rot_33
    // If bStabilizeYaw is true, component from game_yaw is removed/countered. Same for pitch/roll.
    // Example: if stabilizing yaw, build a rotation matrix with only game_pitch and game_roll.
    // This is a simplification. Hydra reconstructs the matrix.
    // For now, m_stabilizedGameCameraRot will just be the inverse of the game's camera rotation if fully stabilized.
    // This is a placeholder for the more complex logic in Hydra.
    if (g_ActiveConfig.bStabilizePitch || g_ActiveConfig.bStabilizeYaw || g_ActiveConfig.bStabilizeRoll) {
        // A proper implementation would extract YPR, selectively zero them out, then reconstruct matrix.
        // As a basic placeholder:
        m_stabilizedGameCameraRot = Common::Matrix44(rot_matrix_33.Inverse());
    } else {
        m_stabilizedGameCameraRot.Identity();
    }

    // TODO: Keyhole logic from Hydra if that feature is desired.
  } else {
    m_stabilizedGameCameraRot.Identity();
    std::memset(m_stabilizedGameCameraPos, 0, sizeof(m_stabilizedGameCameraPos));
  }
  this->dirty = true; // Constants may have changed
}

void VertexShaderManager::CheckSkybox()
{
  // Ported from Hydra's VertexShaderManager::CheckSkybox
  // This sets m_is_skybox based on current projection and modelview matrix.
  m_is_skybox = false; // Default
  if (xfmem.projection.type == ProjectionType::Perspective)
  {
    // GetCurrentGameModelViewMatrix() gets it from xfmem.posMatrices based on CP state.
    // Hydra used constants.posnormalmatrix which is derived from the same source.
    Common::Matrix44 game_mv = GetCurrentGameModelViewMatrix();

    // Check if translation part is near zero
    Common::Vec3 translation = game_mv.GetTranslation();
    if (translation.LengthSquared() < 0.01f) // Hydra used (pos[0]==0 && pos[1]==0 && pos[2]==0)
    {
        // Check if it's not an identity matrix (which could be a UI element at origin)
        // Hydra checked if p[0*4+0] != 1.0f. A more robust check might be needed.
        // An identity modelview matrix at origin could be a fullscreen quad for effects.
        // A skybox usually has rotation but no translation or scaling.
        Common::Matrix33 rot_part = game_mv.ToMatrix33();
        float scale_x = Common::Vec3(rot_part(0,0), rot_part(1,0), rot_part(2,0)).Length();
        // If scale is close to 1.0 and it's not pure identity rotation.
        if (std::abs(scale_x - 1.0f) < 0.1f)
        {
            if (!rot_part.IsIdentity(0.01f)) // Not strictly identity
            {
                 m_is_skybox = true;
            }
            // Hydra's simple check: (p[0*4+0] != 1.0f), assuming if [0][0] is not 1, it's rotated.
            // This might be too simple.
            // A common skybox pattern is modelview matrix is identity or rotation-only, and centered at origin.
            // And projection is perspective.
            // The current check: perspective, at origin, scale ~1, not identity rotation -> likely skybox.
        }
    }
  }
  // Additionally, Hydra's SetViewportType had a check:
  // if (znear >= 0.99f && zfar >= 0.999f) g_is_skybox = true;
  // This could be added here if Viewport struct (from xfmem) is readily available and parsed.
  // const Viewport& vp = xfmem.viewport;
  // float znear_vp = (vp.farZ - vp.zRange) / 16777216.0f; // Max Z value
  // float zfar_vp = vp.farZ / 16777216.0f;
  // if (znear_vp >= 0.99f && zfar_vp >= 0.999f) m_is_skybox = true;

  if (m_is_skybox) {
    m_viewport_type = VIEW_SKYBOX; // Override viewport type if skybox detected
  }
  this->dirty = true;
}

void VertexShaderManager::LockSkybox()
{
  // Ported from Hydra's VertexShaderManager::LockSkybox
  // If iMotionSicknessSkybox == 2, this locks the skybox's modelview matrix.
  if (xfmem.projection.type == ProjectionType::Perspective && g_ActiveConfig.iMotionSicknessSkybox == 2)
  {
    // Get current game modelview matrix (which should be the skybox's at this point)
    Common::Matrix44 game_mv = GetCurrentGameModelViewMatrix();

    if (m_had_skybox_locked)
    {
      // If already locked, subsequent calls might try to overwrite constants.posnormalmatrix.
      // This function's purpose is to fill m_locked_skybox_matrix ONCE,
      // and then ApplySkyboxTransformations will USE m_locked_skybox_matrix.
      // So, this function primarily ensures m_locked_skybox_matrix is captured correctly.
      // The actual "using" of the locked matrix happens in ApplySkyboxTransformations.
      // No need to modify constants.posnormalmatrix here.
    }
    else
    {
      // Store the 3x4 part of the current modelview matrix
      m_locked_skybox_matrix[0] = game_mv.mData[0][0]; m_locked_skybox_matrix[1] = game_mv.mData[0][1]; m_locked_skybox_matrix[2] = game_mv.mData[0][2]; m_locked_skybox_matrix[3] = game_mv.mData[0][3];
      m_locked_skybox_matrix[4] = game_mv.mData[1][0]; m_locked_skybox_matrix[5] = game_mv.mData[1][1]; m_locked_skybox_matrix[6] = game_mv.mData[1][2]; m_locked_skybox_matrix[7] = game_mv.mData[1][3];
      m_locked_skybox_matrix[8] = game_mv.mData[2][0]; m_locked_skybox_matrix[9] = game_mv.mData[2][1]; m_locked_skybox_matrix[10] = game_mv.mData[2][2];m_locked_skybox_matrix[11] = game_mv.mData[2][3];
      m_had_skybox_locked = true;
    }
  }
  // No direct change to 'dirty' here as this just captures state.
  // ApplySkyboxTransformations will set dirty if it uses this and changes projections.
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
void VertexShaderManager::SetConstants(const std::vector<std::string>& textures,
                                       XFStateManager& xf_state_manager)
{
  // --- Existing logic to update constants from xf_state_manager ---
  // This ensures game-driven XF state (lights, materials, texmatrices, etc.) is still loaded.
  // Some of this might be overridden or augmented by VR logic later.

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

  // --- VR Overhaul Starts ---
  bool projection_changed_this_frame = xf_state_manager.DidProjectionChange() || g_freelook_camera.GetController()->IsDirty() || m_projection_graphics_mod_change;

  // Load the game's base projection matrix (with aspect hacks but without freelook/VR)
  // This is stored in m_gameProjectionMatrix
  LoadGameProjectionMatrix(xf_state_manager);
  if(xf_state_manager.DidProjectionChange()) // Reset flag after LoadGameProjectionMatrix potentially uses it
  {
      xf_state_manager.ResetProjectionChangeFlag();
      projection_changed_this_frame = true; // Ensure it's true if ResetProjectionChangeFlag was called
  }


  // Perform draw call classification
  ClassifyCurrentDrawCall(xf_state_manager); // Sets m_viewport_type, m_is_skybox

  // Update free-look camera (its matrices are m_viewRotationMatrix, m_viewTranslationVector)
  // FreeLook::Update() is usually called elsewhere per frame if active.
  // Here we just use its state if g_freelook_camera.IsActive().

  if (g_ActiveConfig.bEnableVR && m_viewport_type != VIEW_RENDER_TO_TEXTURE)
  {
    // Get current game model-view matrix
    Common::Matrix44 game_modelview_matrix = GetCurrentGameModelViewMatrix();

    if (m_is_skybox)
    {
      ApplySkyboxTransformations(); // Uses members: m_gameProjectionMatrix, game_modelview_matrix (implicitly via GetCurrent), VR HMD data
    }
    else if (IsConsideredHUD(m_viewport_type))
    {
      ApplyHUDTransformations(); // Uses members: m_gameProjectionMatrix, game_modelview_matrix, VR HMD data, HUD config
    }
    else // 3D World
    {
      ApplyWorldTransformations(); // Uses members: m_gameProjectionMatrix, game_modelview_matrix, VR HMD data, world scale, free look, stabilization
    }

    // After transformations, constants.projection should be set to one of the eye projections (e.g. left)
    // And constants.stereoparams should be set for the Geometry Shader
  }
  else // Standard path (No VR, or Render-To-Texture which usually bypasses VR view)
  {
    // This is similar to the existing SetProjectionMatrix logic
    if (projection_changed_this_frame || this->dirty) // Ensure projection is updated if dirty for other reasons
    {
        Common::Matrix44 proj_for_shader = Common::Matrix44::Identity();
        if (g_freelook_camera.IsActive()) {
            // Current LoadProjectionMatrix applies freelook and aspect hacks.
            // We need m_gameProjectionMatrix (which already has aspect hacks) and then apply freelook.
            proj_for_shader = m_gameProjectionMatrix * g_freelook_camera.GetView();
        } else {
            proj_for_shader = m_gameProjectionMatrix;
        }

        // Graphics mods can alter projection
        GraphicsModActionData::Projection projection_data{&proj_for_shader};
        for (const auto& action : g_graphics_mod_manager->GetProjectionActions(xfmem.projection.type))
        {
            action->OnProjection(&projection_data);
        }
        // TODO: Handle texture-specific projection mods if any.

        memcpy(constants.projection.data(), proj_for_shader.GetData(), 4 * sizeof(float4));

        // For mono or non-VR stereo, set both eye projections the same initially
        memcpy(eye_projection_left, proj_for_shader.GetData(), 4 * sizeof(float4));
        memcpy(eye_projection_right, proj_for_shader.GetData(), 4 * sizeof(float4));

        if (g_ActiveConfig.stereo_mode != StereoMode::Off && !g_ActiveConfig.bEnableVR) {
            // Apply non-VR anaglyph/SBS/TAB stereo adjustments if needed (usually done in post-processing or GS)
            // For now, UpdateStereoParamsForGS will set basic stereo params for GS if stereo is on.
        }
        UpdateStereoParamsForGS(); // Set stereoparams for GS even in non-VR stereo
        this->dirty = true;
    }
  }

  // Viewport constants (pixel center correction, etc.)
  // This needs to be called after m_gameProjectionMatrix is stable for the frame
  // and after VR viewport decisions might have been made.
  // Hydra called SetViewportConstants() within its SetConstants().
  if (xf_state_manager.DidViewportChange() || projection_changed_this_frame || this->dirty) { // If viewport or projection changed
      SetViewportConstants(); // Sets constants.pixelcentercorrection, constants.viewport
      xf_state_manager.ResetViewportChangeFlag();
      BPFunctions::SetScissorAndViewport(); // This was also called from old SetConstants
      g_stats.AddScissorRect();
      this->dirty = true;
  }

  // Ensure dirty is true if any significant re-calculation happened.
  // The individual Apply...Transformations methods also set this->dirty.
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

// --- Start of new/adapted VR/Hydra methods ---

void VertexShaderManager::SetViewportChanged()
{
  this->dirty = true;
}

void VertexShaderManager::SetProjectionChanged()
{
  this->dirty = true;
}

void VertexShaderManager::TranslateView(float left_metres, float forward_metres, float down_metres)
{
  float vector[3] = {left_metres, down_metres, forward_metres};
  // TODO: Apply world scale (UnitsPerMetre from config, g_ActiveConfig.fScale)
  float result[3];
  m_viewInvRotationMatrix.Multiply(vector, result);

  for (size_t i = 0; i < 3; i++)
    m_viewTranslationVector[i] += result[i];

  SetProjectionChanged();
}

void VertexShaderManager::RotateView(float x_rad, float y_rad)
{
  m_viewRotation[0] += y_rad; // Yaw
  m_viewRotation[1] += x_rad; // Pitch

  constexpr float half_pi = static_cast<float>(M_PI_2);
  m_viewRotation[1] = std::max(-half_pi + 0.001f, std::min(m_viewRotation[1], half_pi - 0.001f));

  Common::Matrix33 matrix_yaw, matrix_pitch;
  matrix_yaw.RotateY(m_viewRotation[0]);
  matrix_pitch.RotateX(m_viewRotation[1]);
  m_viewRotationMatrix = matrix_yaw * matrix_pitch;

  m_viewInvRotationMatrix = m_viewRotationMatrix;
  if (!m_viewInvRotationMatrix.Invert())
  {
    m_viewInvRotationMatrix.Identity();
    ERROR_LOG_FMT(VIDEO, "View matrix inversion failed in RotateView");
  }
  SetProjectionChanged();
}

void VertexShaderManager::ScaleView(float scale)
{
  for (int i = 0; i < 3; i++)
    m_viewTranslationVector[i] *= scale;
  SetProjectionChanged();
}

void VertexShaderManager::ClassifyCurrentDrawCall(XFStateManager& xf_state_manager)
{
  m_old_viewport_type = m_viewport_type;

  const Viewport& vp = xfmem.viewport;
  float vp_width = 2.0f * vp.wd;
  float vp_height = -2.0f * vp.ht;

  float target_width = static_cast<float>(g_framebuffer_manager->GetEFBWidth());
  float target_height = static_cast<float>(g_framebuffer_manager->GetEFBHeight());

  // Basic fullscreen check
  if (std::abs(vp.xOrig - vp.wd - EFB_WIDTH/2.0f) < 2.0f && // Center X approx EFB_WIDTH/2
      std::abs(vp.yOrig + vp.ht - EFB_HEIGHT/2.0f) < 2.0f && // Center Y approx EFB_HEIGHT/2
      std::abs(vp_width - target_width) < 2.0f &&
      std::abs(std::abs(vp_height) - target_height) < 2.0f)
  {
    m_viewport_type = VIEW_FULLSCREEN;
  }
  else
  {
    // TODO: More sophisticated classification for letterbox, pillarbox, splitscreen, specific HUD rects
    m_viewport_type = VIEW_HUD_ELEMENT;
  }

  bool old_skybox_status = m_is_skybox;
  m_is_skybox = false;
  if (g_ActiveConfig.bDetectSkybox && (m_viewport_type == VIEW_FULLSCREEN || m_viewport_type == VIEW_LETTERBOXED))
  {
      CheckSkybox();
  }
  if (m_is_skybox) m_viewport_type = VIEW_SKYBOX;

  if (m_old_viewport_type != m_viewport_type || old_skybox_status != m_is_skybox) {
    this->dirty = true;
  }
}

bool VertexShaderManager::IsConsideredHUD(ViewportType type) const
{
  return type == VIEW_HUD_ELEMENT ||
         type == VIEW_LETTERBOXED ||
        (type >= VIEW_PLAYER_1 && type <= VIEW_PLAYER_4 && g_ActiveConfig.bHudFullscreen);
}

// LoadGameProjectionMatrix is already defined.
// GetCurrentGameModelViewMatrix is already defined.

void VertexShaderManager::ApplySkyboxTransformations()
{
    // Placeholder - actual logic from Hydra to be filled in
    CalculateStereoProjectionsAndViewports(m_gameProjectionMatrix); // Base it on game projection for now
    UpdateStereoParamsForGS();
    // Typically, use one of the eye projections for constants.projection
    memcpy(constants.projection.data(), m_eyeProjectionLeft.GetData(), 4 * sizeof(float4));
    this->dirty = true;
}

void VertexShaderManager::ApplyHUDTransformations()
{
    // Placeholder - actual logic from Hydra to be filled in
    CalculateStereoProjectionsAndViewports(Common::Matrix44::Identity()); // HUD often uses ortho or custom perspective
    UpdateStereoParamsForGS();
    memcpy(constants.projection.data(), m_eyeProjectionLeft.GetData(), 4 * sizeof(float4));
    this->dirty = true;
}

void VertexShaderManager::ApplyWorldTransformations()
{
    // Placeholder - actual logic from Hydra to be filled in
    CalculateStereoProjectionsAndViewports(m_gameProjectionMatrix);
    UpdateStereoParamsForGS();
    memcpy(constants.projection.data(), m_eyeProjectionLeft.GetData(), 4 * sizeof(float4));
    this->dirty = true;
}

void VertexShaderManager::CalculateStereoProjectionsAndViewports(const Common::Matrix44& base_projection)
{
    // TODO: Get HMD projection matrices from VR system (e.g. OpenVR via VR::GetProjectionMatrices)
    m_eyeProjectionLeft = base_projection;
    m_eyeProjectionRight = base_projection;

    // This is where VR_GetProjectionMatrices(final_matrix_left, final_matrix_right, znear, zfar); from Hydra would be called
    // And then results stored into m_eyeProjectionLeft, m_eyeProjectionRight
    // For now, they are just copies of the base_projection.

    memcpy(eye_projection_left, m_eyeProjectionLeft.GetData(), 4 * sizeof(float4));
    memcpy(eye_projection_right, m_eyeProjectionRight.GetData(), 4 * sizeof(float4));
    this->dirty = true;
}

void VertexShaderManager::UpdateStereoParamsForGS()
{
    // constants.stereoparams must be populated based on m_eyeProjectionLeft/Right
    // This is what Geometry Shader will use.
    // Example:
    // constants.stereoparams.x = m_eyeProjectionLeft.mData[0][3]; // Assuming this is horizontal component of projection
    // constants.stereoparams.y = m_eyeProjectionRight.mData[0][3];
    // constants.stereoparams.z = some_depth_related_factor; // e.g. g_ActiveConfig.fStereoConvergence
    // constants.stereoparams.w = 0.0f; // Or another factor

    // Placeholder values:
    constants.stereoparams.x = -0.03f * g_ActiveConfig.fScale;
    constants.stereoparams.y = 0.03f * g_ActiveConfig.fScale;
    constants.stereoparams.z = 1.0f * g_ActiveConfig.fScale;
    constants.stereoparams.w = 0.0f;
    this->dirty = true;
}

void VertexShaderManager::CheckOrientationConstants() { /* TODO: Implement from Hydra if camera stabilization is ported */ }
void VertexShaderManager::CheckSkybox()
{
    // Simplified from Hydra for now
    Common::Matrix44 mv = GetCurrentGameModelViewMatrix();
    // Check if modelview matrix is close to identity (or only rotation) and at origin
    if (xfmem.projection.type == ProjectionType::Perspective)
    {
        bool is_identity_ish = true;
        for(int r=0; r<3; ++r) for(int c=0; c<3; ++c) {
            if(std::abs(mv.mData[r][c] - (r==c ? 1.0f : 0.0f)) > 0.1f) { // Allow some tolerance for rotation-only
                // This check is too simple, a pure rotation is not identity.
                // A better check is if the translation part is zero.
            }
        }
        if(mv.GetTranslation().LengthSquared() < 0.01f) { // Close to origin
             // And if XF registers for modelview matrix indicate it's primarily rotation (e.g. scale is 1)
            float scale_x = Common::Vec3(mv.mData[0][0],mv.mData[1][0],mv.mData[2][0]).Length();
             if (std::abs(scale_x - 1.0f) < 0.1f) { // Assuming uniform scale
                m_is_skybox = true;
             }
        }
    }
}
void VertexShaderManager::LockSkybox() { /* TODO: Implement from Hydra */ }

// --- End of new/adapted VR/Hydra methods ---
