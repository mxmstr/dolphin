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
  m_old_viewport_type = m_viewport_type;

  // Simplified placeholder logic. Full logic from Hydra is complex.
  // It involves checking xfmem.viewport against screen dimensions (e.g. g_renderer->GetTargetWidth/Height())
  // and xfmem.projection properties.
  const Viewport& vp = xfmem.viewport;
  float vp_width = 2.0f * vp.wd;
  float vp_height = -2.0f * vp.ht; // ht is typically negative in GX

  // These would ideally come from a more reliable source for the current render target.
  // Using FramebufferManager for EFB dimensions is safer.
  float target_width = static_cast<float>(g_framebuffer_manager->GetEFBWidth());
  float target_height = static_cast<float>(g_framebuffer_manager->GetEFBHeight());

  if (std::abs(vp_width - target_width) < 2.0f && std::abs(std::abs(vp_height) - target_height) < 2.0f && vp.xOrig - vp.wd < 2.0f && vp.yOrig + vp.ht < 2.0f) // Check origin too
  {
    m_viewport_type = VIEW_FULLSCREEN;
  }
  else
  {
    // Further classification (letterbox, splitscreen, HUD element) needed here.
    // For now, defaulting non-fullscreen to HUD_ELEMENT.
    m_viewport_type = VIEW_HUD_ELEMENT;
  }

  bool old_skybox_status = m_is_skybox;
  m_is_skybox = false; // Default
  if (g_ActiveConfig.bDetectSkybox && m_viewport_type == VIEW_FULLSCREEN) // Skybox usually fullscreen
  {
      CheckSkybox(); // This method will set m_is_skybox if conditions are met
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
    // TODO: Implement based on Hydra
    this->dirty = true;
}

void VertexShaderManager::ApplyHUDTransformations()
{
    // TODO: Implement based on Hydra
    this->dirty = true;
}

void VertexShaderManager::ApplyWorldTransformations()
{
    // TODO: Implement based on Hydra
    this->dirty = true;
}

void VertexShaderManager::CalculateStereoProjectionsAndViewports(const Common::Matrix44& base_projection)
{
    m_eyeProjectionLeft = base_projection;
    m_eyeProjectionRight = base_projection;

    // TODO: Integrate VR_GetProjectionMatrices (or equivalent OpenVR call) here.
    // This will modify m_eyeProjectionLeft and m_eyeProjectionRight based on HMD properties.
    // Example: VR::GetProjectionMatrices(m_eyeProjectionLeft, m_eyeProjectionRight, znear, zfar);
    // where znear/zfar might be derived from base_projection or game settings.

    // Store them in the float4 arrays for legacy paths if needed.
    // The main path will use the Matrix44 versions.
    memcpy(eye_projection_left, m_eyeProjectionLeft.GetData(), 4 * sizeof(float4));
    memcpy(eye_projection_right, m_eyeProjectionRight.GetData(), 4 * sizeof(float4));
    this->dirty = true;
}

void VertexShaderManager::UpdateStereoParamsForGS()
{
    // Derive constants.stereoparams for Geometry Shader based on m_eyeProjectionLeft/Right.
    // This is highly dependent on how the HMD projection affects the matrices.
    // A common way is to find the horizontal shift component in the projection matrix.
    // Example (very simplified, assumes m_eyeProjection matrices are set):
    // If projection matrix P has P[0][3] as horizontal shift for left eye, P[0][3] for right eye:
    // constants.stereoparams.x = m_eyeProjectionLeft.mData[0][3];
    // constants.stereoparams.y = m_eyeProjectionRight.mData[0][3];
    // constants.stereoparams.z = some_convergence_or_depth_factor; (e.g., g_ActiveConfig.fStereoConvergence / m_world_scale)
    // constants.stereoparams.w = 0.0f; // Often unused

    // Using placeholder values:
    constants.stereoparams.x = -0.03f * g_ActiveConfig.fScale; // Example: IPD/2 related offset in clip space
    constants.stereoparams.y = 0.03f * g_ActiveConfig.fScale;
    constants.stereoparams.z = 1.0f * g_ActiveConfig.fScale; // Example convergence plane distance in world units (if shaders use it like that)
    constants.stereoparams.w = 0.0f;
    this->dirty = true;
}

void VertexShaderManager::CheckOrientationConstants() { /* TODO: Implement from Hydra if camera stabilization is ported */ }
void VertexShaderManager::CheckSkybox()
{
    // TODO: Implement full skybox detection logic from Hydra.
    // Involves checking if current model-view matrix is identity or only rotation, centered at origin.
    // And if projection is perspective.
    // For now, keeps m_is_skybox as false unless set by more specific logic.
    // Example simplified check:
    // Common::Matrix44 mv = GetCurrentGameModelViewMatrix();
    // if (xfmem.projection.type == ProjectionType::Perspective && mv.IsIdentity() && (mv.GetTranslation().LengthSquared() < 0.001f)) {
    //    m_is_skybox = true;
    // }
}
void VertexShaderManager::LockSkybox() { /* TODO: Implement from Hydra, uses m_locked_skybox_matrix, m_had_skybox_locked */ }


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
  // TODO: Apply world scale if vector is in meters.
  // TODO: Ensure m_viewInvRotationMatrix is correctly maintained by RotateView.

  float result[3];
  m_viewInvRotationMatrix.Multiply(vector, result);

  for (size_t i = 0; i < 3; i++)
    m_viewTranslationVector[i] += result[i];

  SetProjectionChanged();
  this->dirty = true;
}

void VertexShaderManager::RotateView(float x_rad, float y_rad)
{
  m_viewRotation[0] += y_rad; // Yaw
  m_viewRotation[1] += x_rad; // Pitch

  // Clamp pitch to +/- 90 degrees to avoid gimbal lock issues / flipping over
  constexpr float half_pi = static_cast<float>(M_PI_2);
  m_viewRotation[1] = std::max(-half_pi + 0.001f, std::min(m_viewRotation[1], half_pi - 0.001f));

  Common::Matrix33 mx, my;
  mx.RotateX(m_viewRotation[1]); // Pitch
  my.RotateY(m_viewRotation[0]); // Yaw

  // Common order: Yaw first, then Pitch
  m_viewRotationMatrix = my * mx;

  m_viewInvRotationMatrix = m_viewRotationMatrix;
  bool inverted = m_viewInvRotationMatrix.Invert(); // Check if inversion was successful
  if (!inverted) {
    // Handle inversion failure, e.g., by setting to identity or logging an error
    m_viewInvRotationMatrix.Identity();
    ERROR_LOG_FMT(VIDEO, "View matrix inversion failed in RotateView");
  }

  SetProjectionChanged();
  this->dirty = true;
}

void VertexShaderManager::ScaleView(float scale)
{
  // This was used in Hydra to scale the view translation vector when world scale changed.
  for (int i = 0; i < 3; i++)
    m_viewTranslationVector[i] *= scale;
  SetProjectionChanged();
  this->dirty = true;
}

// ResetView was added to Init() in a previous step, this is its standalone definition
// void VertexShaderManager::ResetView() // Already defined and called by Init

// --- VR Helper Method Stubs ---
void VertexShaderManager::ClassifyCurrentDrawCall(XFStateManager& xf_state_manager)
{
  m_old_viewport_type = m_viewport_type;
  // TODO: Full classification logic based on xfmem.viewport, m_gameProjectionMatrix, g_ActiveConfig, render target size
  m_viewport_type = VIEW_FULLSCREEN;
  m_is_skybox = false;
  // if (g_ActiveConfig.bDetectSkybox && ...) { CheckSkybox(); }
  if (m_old_viewport_type != m_viewport_type || m_is_skybox != (m_old_viewport_type == VIEW_SKYBOX)) { // Basic change detection
    this->dirty = true;
  }
}

bool VertexShaderManager::IsConsideredHUD(ViewportType type) const
{
  return type == VIEW_HUD_ELEMENT || type == VIEW_LETTERBOXED;
}

Common::Matrix44 VertexShaderManager::LoadGameProjectionMatrix(XFStateManager& xf_state_manager)
{
  if (xf_state_manager.DidProjectionChange() || m_gameProjectionMatrix == Common::Matrix44::Identity()) // Simplified load condition
  {
    const auto& rawProj = xfmem.projection;
    float p0 = rawProj.rawProjection[0], p1 = rawProj.rawProjection[1], p2 = rawProj.rawProjection[2],
          p3 = rawProj.rawProjection[3], p4 = rawProj.rawProjection[4], p5 = rawProj.rawProjection[5];

    switch (rawProj.type)
    {
    case ProjectionType::Perspective:
      m_gameProjectionMatrix = Common::Matrix44(p0,0,p1,0, 0,p2,p3,0, 0,0,p4,p5, 0,0,-1,0);
      break;
    case ProjectionType::Orthographic:
      m_gameProjectionMatrix = Common::Matrix44(p0,0,0,p1, 0,p2,0,p3, 0,0,p4,p5, 0,0,0,1);
      break;
    default:
      m_gameProjectionMatrix.Identity();
      ERROR_LOG_FMT(VIDEO, "Unknown game projection type: {}", rawProj.type);
      break;
    }
    if (xfmem.projection.type == ProjectionType::Perspective) {
        // Apply aspect hack directly to the game projection matrix that will be used as base for VR
        m_gameProjectionMatrix.mData[0][0] *= g_ActiveConfig.fAspectRatioHackW;
        m_gameProjectionMatrix.mData[0][2] *= g_ActiveConfig.fAspectRatioHackW;
        m_gameProjectionMatrix.mData[1][1] *= g_ActiveConfig.fAspectRatioHackH;
        m_gameProjectionMatrix.mData[1][2] *= g_ActiveConfig.fAspectRatioHackH;
    }
    xf_state_manager.ResetProjectionChangeFlag();
    this->dirty = true;
  }
  return m_gameProjectionMatrix;
}

Common::Matrix44 VertexShaderManager::GetCurrentGameModelViewMatrix()
{
    const u32 matrix_index = g_main_cp_state.matrix_index_a.PosNormalMtxIdx;
    const float* mv_ptr = &xfmem.posMatrices[matrix_index * 4];
    return Common::Matrix44(mv_ptr[0], mv_ptr[1], mv_ptr[2], mv_ptr[3],
                            mv_ptr[4], mv_ptr[5], mv_ptr[6], mv_ptr[7],
                            mv_ptr[8], mv_ptr[9], mv_ptr[10], mv_ptr[11],
                            0.0f,      0.0f,      0.0f,       1.0f);
}

void VertexShaderManager::ApplySkyboxTransformations()
{
    this->dirty = true;
}

void VertexShaderManager::ApplyHUDTransformations()
{
    this->dirty = true;
}

void VertexShaderManager::ApplyWorldTransformations()
{
    this->dirty = true;
}

void VertexShaderManager::CalculateStereoProjectionsAndViewports(const Common::Matrix44& base_projection)
{
    // TODO: Get HMD projection matrices from VR system
    // For now, just use base for both eyes, then apply simple offset for stereoparams
    m_eyeProjectionLeft = base_projection;
    m_eyeProjectionRight = base_projection;

    // Store them in the float4 arrays if still needed by some path
    memcpy(eye_projection_left, m_eyeProjectionLeft.GetData(), 4 * sizeof(float4));
    memcpy(eye_projection_right, m_eyeProjectionRight.GetData(), 4 * sizeof(float4));
    this->dirty = true;
}

void VertexShaderManager::UpdateStereoParamsForGS()
{
    // This should derive values from m_eyeProjectionLeft/Right or HMD properties
    // Example: if eye projections are simple translations of a center proj:
    // float ipd_ndc_offset = ... ; // calculated based on IPD and projection
    // constants.stereoparams.x = -ipd_ndc_offset;
    // constants.stereoparams.y = ipd_ndc_offset;
    // constants.stereoparams.z = some_convergence_or_depth_factor;
    // constants.stereoparams.w = 0.0f;

    // Using placeholder values from previous attempt for now:
    constants.stereoparams.x = -0.05f;
    constants.stereoparams.y = 0.05f;
    constants.stereoparams.z = 1.0f;
    constants.stereoparams.w = 0.0f;
    this->dirty = true;
}

// Stubs for CheckOrientationConstants, CheckSkybox, LockSkybox
void VertexShaderManager::CheckOrientationConstants() {}
void VertexShaderManager::CheckSkybox()
{
    // Placeholder: actual skybox detection is complex
    // Example: if (xfmem.projection.type == GX_PERSPECTIVE) {
    //   Common::Matrix44 mv = GetCurrentGameModelViewMatrix();
    //   if (mv.IsTranslationOnly() && mv.GetTranslation() == Common::Vec3(0,0,0)) {
    //     m_is_skybox = true; // Simplified
    //   }
    // }
}
void VertexShaderManager::LockSkybox() {}


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
