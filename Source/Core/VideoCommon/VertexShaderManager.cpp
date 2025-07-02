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

// Definitions for externs from header
ViewportType g_viewport_type = VIEW_FULLSCREEN;
bool g_is_skybox = false;
EFBRectangle g_final_screen_region = EFBRectangle(0, 0, EFB_WIDTH, EFB_HEIGHT); // Default EFB size

// Definitions for static members from header
Common::Matrix44 VertexShaderManager::constants_eye_projection[2];
float VertexShaderManager::s_locked_skybox[12];
bool VertexShaderManager::s_had_skybox = false;
Common::Matrix44 VertexShaderManager::g_game_camera_rotmat = Common::Matrix44::Identity();
float VertexShaderManager::g_game_camera_pos[3] = {0.0f, 0.0f, 0.0f};


void VertexShaderManager::Init()
{
  // Initialize state tracking variables
  m_projection_graphics_mod_change = false;

  constants = {};

  m_projection_matrix = Common::Matrix44::Identity().data;

  // Initialize VR specific static variables
  s_had_skybox = false;
  std::memset(s_locked_skybox, 0, sizeof(s_locked_skybox));
  g_game_camera_rotmat = Common::Matrix44::Identity();
  g_game_camera_pos[0] = g_game_camera_pos[1] = g_game_camera_pos[2] = 0.0f;
  constants_eye_projection[0] = Common::Matrix44::Identity();
  constants_eye_projection[1] = Common::Matrix44::Identity();
  g_viewport_type = VIEW_FULLSCREEN;
  g_is_skybox = false;
  // g_final_screen_region is already initialized at definition

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


// VR-Hydra Free Look Camera functions (adapted interface, stubs for now)
// These would typically interact with g_freelook_camera and mark VR projections as dirty.
// These are now member functions as per the header.
void VertexShaderManager::TranslateView(float /*left_metres*/, float /*forward_metres*/, float /*down_metres*/)
{
  // TODO: Implement by calling g_freelook_camera methods
  // e.g., g_freelook_camera.Move(left, forward, down);
  // And then ensure VR projection constants will be recalculated.
  // For now, just mark general dirty flag.
  // dirty is a member, so this->dirty or just dirty if in a member function.
  // However, these are static in Hydra's header. If they remain static, they can't modify member 'dirty'.
  // Let's assume they are static for now as per Hydra's design, meaning they'd need to operate on static dirty flags or trigger XFStateManager.
  // For simplicity, let's assume a global or static dirty flag for VR constants exists or XFStateManager handles it.
  // If these become non-static, they can use this->dirty.
  // The header for Reloaded has Init/DoState as non-static, others static.
  // For now, assuming these helpers would also be static if they were direct ports.
  // The current Reloaded header doesn't have these, so this is more for future.
  // If these are added to the class, they'd likely be static to match Hydra.
  // For now, let's assume they'd trigger a global recalculation if they were implemented.
  if (g_graphics_mod_manager)
    g_graphics_mod_manager->SetCameraDirty(); // This is a good generic way to signal camera change
}

void VertexShaderManager::RotateView(float /*x_degrees*/, float /*y_degrees*/)
{
  if (g_graphics_mod_manager)
    g_graphics_mod_manager->SetCameraDirty();
}

void VertexShaderManager::ScaleView(float /*scale*/)
{
  if (g_graphics_mod_manager)
    g_graphics_mod_manager->SetCameraDirty();
}

void VertexShaderManager::ResetView()
{
  g_freelook_camera.Reset();
  s_had_skybox = false; // Reset skybox lock on view reset (s_had_skybox is static)
  if (g_graphics_mod_manager)
    g_graphics_mod_manager->SetCameraDirty();
}


// START VR-Hydra adapted helper functions & VR Projection Logic

// Simplified from Hydra - focuses on matrix operations for skybox
static void LockSkyboxMatrix(Common::Matrix44& current_pos_normal_matrix)
{
    // If a skybox was previously identified and its matrix locked
    if (VertexShaderManager::s_had_skybox)
    {
        // Overwrite the current posnormalmatrix's rotational part with the locked one.
        // s_locked_skybox stores 3x4 matrix (rotation + translation)
        // We only want to restore the 3x3 rotation part here.
        // The translation part for skyboxes should ideally remain zero.
        float* pnm_data = current_pos_normal_matrix.data.data();
        for(int row = 0; row < 3; ++row) {
            for(int col = 0; col < 3; ++col) { // Only copy 3x3 part
                pnm_data[row * 4 + col] = VertexShaderManager::s_locked_skybox[row * 4 + col];
            }
            pnm_data[row * 4 + 3] = 0.0f; // Ensure translation is zero
        }
    }
    else
    {
        // Lock the current matrix if it's identified as a skybox draw
        // Ensure only the 3x3 rotation part is locked, translation should be zero for skyboxes.
        const float* pnm_data = VertexShaderManager::constants.posnormalmatrix[0].data(); // Assuming this is the current one
        for(int row = 0; row < 3; ++row) {
            for(int col = 0; col < 3; ++col) {
                 VertexShaderManager::s_locked_skybox[row * 4 + col] = pnm_data[row * 4 + col];
            }
            VertexShaderManager::s_locked_skybox[row * 4 + 3] = 0.0f;
        }
        VertexShaderManager::s_had_skybox = true;
    }
}

// Placeholder for CheckOrientationConstants - In Hydra, this read game camera from matrix.
// For now, it will just use g_game_camera_pos/rotmat which are assumed to be updated elsewhere if stabilization is active.
static void ApplyGameCameraStabilization(Common::Matrix44& view_matrix)
{
    if (g_ActiveConfig.bStabilizeX || g_ActiveConfig.bStabilizeY || g_ActiveConfig.bStabilizeZ ||
        g_ActiveConfig.bStabilizePitch || g_ActiveConfig.bStabilizeYaw || g_ActiveConfig.bStabilizeRoll)
    {
        Common::Matrix44 game_cam_translation_matrix = Common::Matrix44::Translate(-VertexShaderManager::g_game_camera_pos[0] / g_ActiveConfig.fUnitsPerMetre,
                                                                               -VertexShaderManager::g_game_camera_pos[1] / g_ActiveConfig.fUnitsPerMetre,
                                                                               -VertexShaderManager::g_game_camera_pos[2] / g_ActiveConfig.fUnitsPerMetre);
        // Inverse of game camera rotation: g_game_camera_rotmat.Transpose() if it's pure rotation
        Common::Matrix44 game_cam_rotation_matrix_inv = VertexShaderManager::g_game_camera_rotmat.Transposed();

        view_matrix = view_matrix * game_cam_translation_matrix * game_cam_rotation_matrix_inv;
    }
}

static Common::Matrix44 CalculateVRViewProjection(const Common::Matrix44& game_projection_matrix, bool is_right_eye)
{
    Common::Matrix44 final_vr_matrix = Common::Matrix44::Identity();
    float UnitsPerMetre = g_ActiveConfig.fUnitsPerMetre / g_ActiveConfig.fScale; // Effective units per meter

    // These would come from VR system (e.g., OpenVR)
    // For now, using placeholders. These need to be properly initialized by the VR system.
    Common::Matrix44 hmd_eye_projection_matrix = game_projection_matrix; // Start with game's, modify for HMD FOV
    Common::Vec3 hmd_eye_offset = Common::Vec3( (is_right_eye ? 0.03f : -0.03f) * UnitsPerMetre, 0.0f, 0.0f ); // Simplified IPD
    Common::Matrix44 head_rotation_matrix = Common::Matrix44::Identity(); // From HMD tracker
    Common::Vec3 head_position_offset = Common::Vec3(0,0,0); // From HMD tracker, in world units


    // 1. Get HMD's projection matrix for the eye (this usually replaces game's FOV and aspect)
    // This is a complex step. Real HMD projection is off-axis and specific.
    // For now, we'll simulate a wider FOV and use the game's near/far.
    // VR_GetProjectionMatrices(hmd_left, hmd_right, znear, zfar) was in Hydra.
    // Let's assume game_projection_matrix is already a base, and we'll adjust it.
    // A proper implementation would get hmd_eye_projection_matrix from the VR SDK.
    // For example, if game_projection_matrix is perspective:
    if (xfmem.projection.type == ProjectionType::Perspective) {
        // Modify hmd_eye_projection_matrix based on VR SDK data (e.g. OpenVR GetProjectionMatrix)
        // This would typically set FOV, aspect, near/far, and off-center elements.
        // Simplified: Slightly increase horizontal FOV by reducing P[0][0]
        // hmd_eye_projection_matrix.data[0] *= 0.8f; // Example: wider FOV
        // hmd_eye_projection_matrix.data[5] *= 0.9f; // Example: adjust vertical if needed
        // The rawProjection values from xfmem.projection are for the game's original setup.
        // p[0]=2n/w, p[1]=A, p[2]=2n/h, p[3]=B, p[4]=-(f+n)/(f-n) (or similar), p[5]=-2fn/(f-n) (or similar for GL)
        // For VR, we need the actual near/far from the game's original projection to pass to SDK.
        float game_near = 0.1f * UnitsPerMetre; // Default if cannot derive
        float game_far = 1000.0f * UnitsPerMetre; // Default

        if (std::abs(game_projection_matrix(2,3)) > 1e-5) { // Check if w component of Z is -1 (GL style)
            if (std::abs(game_projection_matrix(2,2) + 1.0f) < 1e-5f && std::abs(game_projection_matrix(3,2)) < 1e-5f ) { // Ortho check based on P[3][2] being 0 for typical perspective
                 // Simpler ortho, take as is for now
            } else {
                 // Standard perspective: P[3][2] is -1 for GL style
                 // P[2][2] = -(f+n)/(f-n), P[2][3] = -2fn/(f-n)
                 float c = game_projection_matrix(2,2); // -(f+n)/(f-n)
                 float d = game_projection_matrix(2,3); // -2fn/(f-n)
                 game_far = d / (c - 1.0f);
                 game_near = d / (c + 1.0f);
            }
        }
        // This is where you'd call something like:
        // hmd_eye_projection_matrix = VR_SDK_GetProjection(is_right_eye, game_near, game_far);
        // For now, we use game_projection_matrix and will apply stereo offsets later if no full SDK matrix.
    }


    // 2. Base View Matrix (World to Camera space for the game)
    // This would be an identity if we are replacing game camera entirely, or game's view matrix.
    // For now, let's assume we start with identity and build up.
    Common::Matrix44 current_view_matrix = Common::Matrix44::Identity();

    // 3. Apply Game Camera Stabilization (if active)
    // This uses g_game_camera_pos and g_game_camera_rotmat
    ApplyGameCameraStabilization(current_view_matrix);

    // 4. Apply Free Look Camera (from g_freelook_camera)
    if (g_freelook_camera.IsActive()) {
        current_view_matrix = current_view_matrix * g_freelook_camera.GetView();
    }

    // 5. Apply HMD Head Rotation and Position (Tracker Space to World Space)
    // Head position is an offset in tracker space, then rotated by head rotation.
    Common::Matrix44 hmd_pose_matrix = head_rotation_matrix * Common::Matrix44::Translate(head_position_offset);
    current_view_matrix = current_view_matrix * hmd_pose_matrix.Inverse(); // Inverse to go from world to HMD view space

    // 6. Apply Eye Offset (HMD space to Eye space)
    Common::Matrix44 eye_offset_matrix = Common::Matrix44::Translate(-hmd_eye_offset.x, -hmd_eye_offset.y, -hmd_eye_offset.z);
    current_view_matrix = eye_offset_matrix * current_view_matrix;


    // Handle Skybox: If it's a skybox, typically you only want HMD rotation, not position or game camera.
    if (g_is_skybox) {
        current_view_matrix = Common::Matrix44::Identity(); // Start fresh for skybox
        if (g_ActiveConfig.iMotionSicknessSkybox == 2) { // World-locked skybox
            // Use locked skybox rotation (s_locked_skybox)
            // s_locked_skybox is 3x4. Convert to Matrix44.
             Common::Matrix44 skybox_model_matrix = Common::Matrix44::Identity();
             for(int r=0; r<3; ++r) for(int c=0; c<4; ++c) skybox_model_matrix(r,c) = s_locked_skybox[r*4+c];
             current_view_matrix = current_view_matrix * skybox_model_matrix.Inverse(); // Game's skybox model to view
        }
        // Apply HMD rotation only
        current_view_matrix = current_view_matrix * head_rotation_matrix.Inverse();
        // Skybox usually uses a special projection or rendered at infinity.
        // For now, use the HMD projection but ensure it's far.
        // hmd_eye_projection_matrix might need near/far adjusted for skybox.
    }
    // Handle HUD: Render as a quad in front of the HMD
    else if (g_viewport_type == VIEW_HUD_ELEMENT || (xfmem.projection.type == ProjectionType::Orthographic && g_viewport_type != VIEW_RENDER_TO_TEXTURE)) {
        float hud_distance = g_ActiveConfig.fHudDistance * UnitsPerMetre;
        float hud_depth = g_ActiveConfig.fHudThickness * UnitsPerMetre; // For stereo separation on HUD
        float hud_scale = g_ActiveConfig.fHudScale; // Overall size scale

        // Use orthographic projection for HUD, scaled and positioned.
        // Dimensions of the HUD quad in world units.
        // Game's ortho projection defines a viewport (left, right, bottom, top).
        // We map this to a quad of size (width * hud_scale, height * hud_scale) at hud_distance.
        float L = xfmem.projection.rawProjection[1]; // game's ortho left
        float R = xfmem.projection.rawProjection[0]; // game's ortho right (derived from raw)
        float B = xfmem.projection.rawProjection[3]; // game's ortho bottom
        float T = xfmem.projection.rawProjection[2]; // game's ortho top (derived from raw)

        // If rawProjection is [scale_x, trans_x, scale_y, trans_y, scale_z, trans_z]
        // right = (1 - trans_x) / scale_x; left = (-1 - trans_x) / scale_x
        // top = (1 - trans_y) / scale_y; bottom = (-1 - trans_y) / scale_y
        // For GX_ORTHOGRAPHIC: P[0]=1/R, P[1]=-(L+R)/(L-R), P[2]=1/T, P[3]=-(T+B)/(T-B) ... (approx)
        // This needs careful mapping from GX ortho to actual world quad dimensions.
        // Simplified: assume game's projection gives a -1 to 1 range that we map to HUD size.
        float hud_world_width = 2.0f * hud_scale; // Default if cannot derive from game proj
        float hud_world_height = 2.0f * hud_scale;
        if (std::abs(R) > 1e-6 && std::abs(T) > 1e-6) { // R and T are scales 2/(right-left) etc.
            hud_world_width = (2.0f / R) * hud_scale;
            hud_world_height = (2.0f / T) * hud_scale;
        }


        // Model matrix for the HUD quad
        Common::Matrix44 hud_model_matrix = Common::Matrix44::Scale(hud_world_width / 2.0f, hud_world_height / 2.0f, 1.0f) *
                                           Common::Matrix44::Translate(0, 0, -hud_distance);
        // View matrix for HUD is just HMD pose (rotation + eye offset)
        current_view_matrix = eye_offset_matrix * head_rotation_matrix.Inverse();

        final_vr_matrix = hmd_eye_projection_matrix * current_view_matrix * hud_model_matrix;

        // Stereo effect for HUD: adjust x in model matrix or view matrix based on eye and hud_depth
        float hud_stereo_offset = (is_right_eye ? 0.5f : -0.5f) * hud_depth * hmd_eye_projection_matrix(0,0); // P[0][0] is related to focal length
        final_vr_matrix(0,3) += hud_stereo_offset * final_vr_matrix(3,3); // Apply parallax to X based on W

        // Set stereoparams for GS (if GS is used for HUD, unlikely but for completeness)
        // For HUD, stereoparams might be simpler: just an X shift.
        GeometryShaderManager::constants.stereoparams[0] = hud_stereo_offset; // Left eye total shift
        GeometryShaderManager::constants.stereoparams[1] = hud_stereo_offset; // Right eye total shift (will be negative if is_right_eye is false)
        GeometryShaderManager::constants.stereoparams[2] = 0; // No off-axis for simple HUD quad
        GeometryShaderManager::constants.stereoparams[3] = 0;
        return final_vr_matrix;
    }

    // Combine: Projection * View
    final_vr_matrix = hmd_eye_projection_matrix * current_view_matrix;

    // Set stereoparams for Geometry Shader
    // These are used by GS to apply per-eye view adjustments if instanced stereo rendering is used.
    // params[0] = LeftEyeProj[0][0] * LeftEyeShiftX - LeftEyeProj[0][2] (for clip space adjustment)
    // params[1] = RightEyeProj[0][0] * RightEyeShiftX - RightEyeProj[0][2]
    // Simplified: Proj[0][0] * EyeOffsetX for world space shift, and Proj[0][2] for off-axis adjustment.
    // The hmd_eye_projection_matrix already contains the off-axis part (P[0][2]).
    // The EyeOffsetX is hmd_eye_offset.x.
    // The GS needs: P00_eye * eye_offset_x (world shift) and P02_eye (off-axis factor)
    if (is_right_eye) {
        GeometryShaderManager::constants.stereoparams[1] = hmd_eye_projection_matrix(0,0) * hmd_eye_offset.x;
        GeometryShaderManager::constants.stereoparams[3] = hmd_eye_projection_matrix(0,2);
    } else {
        GeometryShaderManager::constants.stereoparams[0] = hmd_eye_projection_matrix(0,0) * hmd_eye_offset.x;
        GeometryShaderManager::constants.stereoparams[2] = hmd_eye_projection_matrix(0,2);
    }
    // This assumes the GS will do: pos.x += stereo_params_world_shift - stereo_params_off_axis * pos.w;

    return final_vr_matrix;
}


// Adapted from VR-Hydra SetViewportType
// Determines the type of viewport and influences g_is_skybox based on depth
static void SetViewportInformation()

// Adapted from VR-Hydra SetViewportType
// Determines the type of viewport and influences g_is_skybox based on depth
static void SetViewportInformation()
{
  // ViewportType old_viewport_type = g_viewport_type; // For comparison if needed later

  // xfmem.viewport values are already in a usable coordinate system.
  // GX hardware adds 342 to raw register values. xfmem.viewport values are derived from these.
  // The crucial part is how they relate to EFB dimensions.
  float x = xfmem.viewport.xOrig - xfmem.viewport.wd;
  float y = xfmem.viewport.yOrig + xfmem.viewport.ht; // GX viewport Y is often top-positive from origin
  float width = 2.0f * xfmem.viewport.wd;
  float height = -2.0f * xfmem.viewport.ht; // GX viewport H is often negative to indicate down

  // Normalize to top-left origin, positive width/height for easier comparison
  if (height < 0)
  {
    y += height; // y becomes top coordinate
    height = -height;
  }
  // If width is negative (can happen if xOrig < wd, though less common for width itself)
  if (width < 0)
  {
    x += width; // x becomes left coordinate
    width = -width;
  }

  // Use EFB dimensions from FramebufferManager as the reference screen size
  float screen_width = static_cast<float>(g_framebuffer_manager->GetEFBWidth());
  float screen_height = static_cast<float>(g_framebuffer_manager->GetEFBHeight());

  // Tolerances for "fullscreen-ish" or "half-screen-ish"
  const float size_tolerance_factor = 0.90f; // e.g., 90% of screen width is "full width"
  const float pos_tolerance_abs = std::max(10.0f, screen_width * 0.05f); // Max deviation from edge

  float min_screen_width_for_full = screen_width * size_tolerance_factor;
  float min_screen_height_for_full = screen_height * size_tolerance_factor;

  // Default to HUD element, then refine
  g_viewport_type = VIEW_HUD_ELEMENT;

  // Heuristic for Render-to-Texture: Often square, power-of-2 (or multiple of 8), and at an edge.
  bool is_square = std::abs(width - height) < 2.0f; // Allow small tolerance for float inaccuracies
  bool is_common_rtt_size = (static_cast<int>(width) % 8 == 0 && width > 4 && width < screen_width && height < screen_height);
  bool at_edge = std::abs(x) < pos_tolerance_abs || std::abs(y) < pos_tolerance_abs ||
                 std::abs(x + width - screen_width) < pos_tolerance_abs ||
                 std::abs(y + height - screen_height) < pos_tolerance_abs;

  if (is_square && is_common_rtt_size && at_edge && !(width > 500 && screen_width > 500 && std::abs(x) < pos_tolerance_abs && std::abs(y) < pos_tolerance_abs) )
  {
    g_viewport_type = VIEW_RENDER_TO_TEXTURE;
  }
  // Fullscreen or Letterboxed
  else if (width >= min_screen_width_for_full && std::abs(x) < pos_tolerance_abs && std::abs(x + width - screen_width) < pos_tolerance_abs)
  {
    if (height >= min_screen_height_for_full && std::abs(y) < pos_tolerance_abs && std::abs(y + height - screen_height) < pos_tolerance_abs)
    {
      g_viewport_type = VIEW_FULLSCREEN;
    }
    // Check for half-height (top/bottom split)
    else if (height >= screen_height * 0.4f && height <= screen_height * 0.6f &&
             (std::abs(y) < pos_tolerance_abs || std::abs(y + height - screen_height) < pos_tolerance_abs))
    {
        g_viewport_type = (y < screen_height / 2.0f) ? VIEW_PLAYER_1 : VIEW_PLAYER_2;
    }
    else if (height < min_screen_height_for_full && height > screen_height * 0.1f)
    {
        g_viewport_type = VIEW_LETTERBOXED;
    }
  }
  // Side-by-side split
  else if (height >= min_screen_height_for_full && std::abs(y) < pos_tolerance_abs && std::abs(y + height - screen_height) < pos_tolerance_abs)
  {
    if (width >= screen_width * 0.4f && width <= screen_width * 0.6f &&
        (std::abs(x) < pos_tolerance_abs || std::abs(x + width - screen_width) < pos_tolerance_abs))
    {
        g_viewport_type = (x < screen_width / 2.0f) ? VIEW_PLAYER_1 : VIEW_PLAYER_2;
    }
  }
  // Quadrants
  else if (width >= screen_width * 0.4f && width <= screen_width * 0.6f &&
           height >= screen_height * 0.4f && height <= screen_height * 0.6f)
  {
    // Check which quadrant it falls into based on x, y
    bool left_half = x < screen_width / 2.0f && (x + width) < (screen_width/2.0f + pos_tolerance_abs);
    bool right_half = x > screen_width / 2.0f - pos_tolerance_abs;
    bool top_half = y < screen_height / 2.0f && (y + height) < (screen_height/2.0f + pos_tolerance_abs);
    bool bottom_half = y > screen_height / 2.0f - pos_tolerance_abs;

    if (left_half && top_half) g_viewport_type = VIEW_PLAYER_1;
    else if (right_half && top_half) g_viewport_type = VIEW_PLAYER_2;
    else if (left_half && bottom_half) g_viewport_type = VIEW_PLAYER_3;
    else if (right_half && bottom_half) g_viewport_type = VIEW_PLAYER_4;
    // else it remains VIEW_HUD_ELEMENT if it doesn't fit quadrant logic cleanly
  }
  // Offscreen check
  else if (x + width <= 0 || x >= screen_width || y + height <= 0 || y >= screen_height)
  {
    g_viewport_type = VIEW_OFFSCREEN;
  }
  // Default: VIEW_HUD_ELEMENT if none of the above.

  // Skybox check based on depth for views that are typically 3D world views
  g_is_skybox = false; // Reset before check
  if (g_viewport_type == VIEW_FULLSCREEN || g_viewport_type == VIEW_LETTERBOXED ||
      (g_viewport_type >= VIEW_PLAYER_1 && g_viewport_type <= VIEW_PLAYER_4))
  {
    float znear_normalized = (xfmem.viewport.farZ - xfmem.viewport.zRange) / 16777216.0f;
    float zfar_normalized = xfmem.viewport.farZ / 16777216.0f;
    if (znear_normalized >= 0.98f && zfar_normalized >= 0.999f && (zfar_normalized - znear_normalized) < 0.01f) { // Slightly relaxed skybox heuristics
      g_is_skybox = true;
    }
  }
}

// From VR-Hydra - Checks if the current model matrix (PosNormalMtx) suggests a skybox
static void CheckSkyboxHeuristic() // Renamed to avoid conflict if a different CheckSkybox exists or is planned
{
  if (g_is_skybox) // Already identified by depth heuristic in SetViewportInformation
      return;

  if (xfmem.projection.type == ProjectionType::Perspective)
  {
    // constants.posnormalmatrix is derived from xfmem.posMatrices and xfmem.normalMatrices for PosNormalMtxIdx
    // This check assumes constants.posnormalmatrix has been updated based on current XF state.
    const float* pnm_matrix_r0 = VertexShaderManager::constants.posnormalmatrix[0].data();
    const float* pnm_matrix_r1 = VertexShaderManager::constants.posnormalmatrix[1].data();
    const float* pnm_matrix_r2 = VertexShaderManager::constants.posnormalmatrix[2].data();

    bool is_translation_zero = (pnm_matrix_r0[3] == 0.0f &&
                                pnm_matrix_r1[3] == 0.0f &&
                                pnm_matrix_r2[3] == 0.0f);

    bool is_not_identity_rotation = (pnm_matrix_r0[0] != 1.0f || pnm_matrix_r0[1] != 0.0f || pnm_matrix_r0[2] != 0.0f ||
                                     pnm_matrix_r1[0] != 0.0f || pnm_matrix_r1[1] != 1.0f || pnm_matrix_r1[2] != 0.0f ||
                                     pnm_matrix_r2[0] != 0.0f || pnm_matrix_r2[1] != 0.0f || pnm_matrix_r2[2] != 1.0f);

    if (is_translation_zero && is_not_identity_rotation)
    {
      // Further check: skybox scale is usually large or non-uniform in a specific way.
      // This is a very rough check; real skyboxes might use various scales.
      // A very large determinant or significantly non-unit scale factors for the 3x3 part.
      float scale_x_sq = pnm_matrix_r0[0]*pnm_matrix_r0[0] + pnm_matrix_r1[0]*pnm_matrix_r1[0] + pnm_matrix_r2[0]*pnm_matrix_r2[0];
      if (scale_x_sq > 10.0f || scale_x_sq < 0.1f) // Arbitrary thresholds indicating non-unit scale
      {
        g_is_skybox = true;
      }
    }
  }
}
// END VR-Hydra adapted helper functions


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

  // Call viewport and skybox classification logic
  // This needs xfmem.viewport (from DidViewportChange)
  // and constants.posnormalmatrix (from DidPosNormalChange) to be up-to-date.
  SetViewportInformation();
  CheckSkyboxHeuristic();

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

    // If VR mode is active (e.g., OpenVR), calculate specific per-eye projections
    if (g_ActiveConfig.stereo_mode == StereoMode::OpenVR) // Assuming OpenVR enum value exists
    {
        Common::Matrix44 left_eye_proj = CalculateVRViewProjection(corrected_matrix, false);
        Common::Matrix44 right_eye_proj = CalculateVRViewProjection(corrected_matrix, true);

        memcpy(constants_eye_projection[0].data.data(), left_eye_proj.data.data(), sizeof(float4) * 4);
        memcpy(constants_eye_projection[1].data.data(), right_eye_proj.data.data(), sizeof(float4) * 4);

        // The main 'constants.projection' can be set to the left eye for compatibility or mono paths
        memcpy(constants.projection.data(), left_eye_proj.data.data(), sizeof(float4) * 4);

        // Ensure GeometryShaderManager constants are also updated if CalculateVRViewProjection set them
        // (It does, so this is mostly a note that they are coupled)
        GeometryShaderManager::dirty = true;
    }
    else
    {
        // Standard mono or other stereo rendering path
        memcpy(constants.projection.data(), corrected_matrix.data.data(), 4 * sizeof(float4));
        // For non-VR stereo modes, clear or set eye projections and GS params appropriately if needed
        constants_eye_projection[0] = corrected_matrix;
        constants_eye_projection[1] = corrected_matrix;
        GeometryShaderManager::constants.stereoparams[0] = 0.0f; // Clear GS stereo params
        GeometryShaderManager::constants.stereoparams[1] = 0.0f;
        GeometryShaderManager::constants.stereoparams[2] = 0.0f;
        GeometryShaderManager::constants.stereoparams[3] = 0.0f;
    }
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

  // VR-Hydra ported variables
  p.Do(constants_eye_projection[0].data); // Common::Matrix44 is not directly serializable, so save its underlying array
  p.Do(constants_eye_projection[1].data);
  p.Do(s_locked_skybox);
  p.Do(s_had_skybox);
  p.Do(g_game_camera_rotmat.data); // Common::Matrix44 is not directly serializable
  p.Do(g_game_camera_pos);
  p.Do(g_viewport_type); // Enum should be serializable as its underlying type
  p.Do(g_is_skybox);
  // g_final_screen_region is EFBRectangle, check if it needs custom serialization or if Do(EFBRectangle) exists.
  // Assuming EFBRectangle is simple enough or PointerWrap handles it.
  p.Do(g_final_screen_region.left);
  p.Do(g_final_screen_region.top);
  p.Do(g_final_screen_region.right);
  p.Do(g_final_screen_region.bottom);


  if (p.IsReadMode())
  {
    dirty = true;
    if (g_graphics_mod_manager) // Ensure mods update on state load
        g_graphics_mod_manager->SetCameraDirty();
  }
}
