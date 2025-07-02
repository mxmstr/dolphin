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
#include "VideoCommon/GeometryShaderManager.h" // Needed for I_STEREOPARAMS
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VR.h" // Will be created later
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"
#include "VideoCommon/XFStateManager.h"

// For DEGREES_TO_RADIANS, etc.
#include "Common/MathUtil.h"


// VR-Hydra helper functions (ported and adapted)
// Declaration for ProjectionHack, PHackValue, g_proj_hack_near, g_proj_hack_far
namespace
{
struct ProjectionHack
{
  float sign;
  float value;
  ProjectionHack() : sign(0.0f), value(0.0f) {} // Initialize
  ProjectionHack(float new_sign, float new_value) : sign(new_sign), value(new_value) {}
};
static ProjectionHack g_proj_hack_near;
static ProjectionHack g_proj_hack_far;

static float PHackValue(const std::string& sValue) // Made const std::string&
{
  float f = 0;
  bool fp = false;
  // Simpler string to float conversion for C++
  try {
    // Check for comma as decimal separator
    std::string temp_sValue = sValue;
    std::replace(temp_sValue.begin(), temp_sValue.end(), ',', '.');
    f = std::stof(temp_sValue);
    fp = temp_sValue.find('.') != std::string::npos;
  } catch (const std::invalid_argument& ia) {
    ERROR_LOG(VIDEO, "Invalid argument for PHackValue: %s", sValue.c_str());
    return 0.0f;
  } catch (const std::out_of_range& oor) {
    ERROR_LOG(VIDEO, "Out of range for PHackValue: %s", sValue.c_str());
    return 0.0f;
  }

  if (!fp && sValue.length() > 0) // If no decimal point and not empty, assume original scaling
    f /= 0xF4240; // Original comment: 1000000.0f;

  return f;
}

} // Anonymous namespace

void UpdateProjectionHack(const ProjectionHackConfig& config) // Assuming ProjectionHackConfig will be defined
{
  float near_value = 0, far_value = 0;
  float near_sign = 1.0, far_sign = 1.0;

  if (config.m_enable)
  {
    // const char* near_sign_str = ""; // Logging detail
    // const char* far_sign_str = "";  // Logging detail

    // NOTICE_LOG(VIDEO, "\t\t--- Orthographic Projection Hack ON ---"); // Use appropriate log macro

    if (config.m_sznear)
    {
      near_sign *= -1.0f;
      // near_sign_str = " * (-1)";
    }
    if (config.m_szfar)
    {
      far_sign *= -1.0f;
      // far_sign_str = " * (-1)";
    }

    near_value = PHackValue(config.m_znear);
    // NOTICE_LOG(VIDEO, "- zNear Correction = (%f + zNear)%s", near_value, near_sign_str);

    far_value = PHackValue(config.m_zfar);
    // NOTICE_LOG(VIDEO, "- zFar Correction =  (%f + zFar)%s", far_value, far_sign_str);
  }
  g_proj_hack_near = ProjectionHack(near_sign, near_value);
  g_proj_hack_far = ProjectionHack(far_sign, far_value);
}


void ScaleRequestedToRendered(const EFBRectangle& requested, EFBRectangle& rendered) // Made const& for input
{
  // Ensure g_requested_viewport dimensions are not zero to prevent division by zero
  if (g_requested_viewport.GetWidth() == 0 || g_requested_viewport.GetHeight() == 0) {
      rendered = requested; // Or some other default behavior
      return;
  }

  float m_w = (float)g_rendered_viewport.GetWidth() / g_requested_viewport.GetWidth();
  rendered.left = (int)(0.5f + (requested.left - g_requested_viewport.left) * m_w + g_rendered_viewport.left);
  rendered.right = (int)(0.5f + (requested.right - g_requested_viewport.left) * m_w + g_rendered_viewport.left);

  float m_h = (float)g_rendered_viewport.GetHeight() / g_requested_viewport.GetHeight();
  rendered.top = (int)(0.5f + (requested.top - g_requested_viewport.top) * m_h + g_rendered_viewport.top);
  rendered.bottom = (int)(0.5f + (requested.bottom - g_requested_viewport.top) * m_h + g_rendered_viewport.bottom);
}

const char* GetViewportTypeName(ViewportType v)
{
  if (g_is_skybox) return "Skybox"; // g_is_skybox is a global from VR port
  switch (v)
  {
  case ViewportType::Fullscreen: return "Fullscreen";
  case ViewportType::Letterboxed: return "Letterboxed";
  case ViewportType::HudElement: return "HUD element";
  case ViewportType::Offscreen: return "Offscreen";
  case ViewportType::RenderToTexture: return "Render to Texture";
  case ViewportType::Player1: return "Player 1";
  case ViewportType::Player2: return "Player 2";
  case ViewportType::Player3: return "Player 3";
  case ViewportType::Player4: return "Player 4";
  // VIEW_SKYBOX was in VR-Hydra's enum, but g_is_skybox bool is used instead now.
  // If ViewportType::Skybox is added to VR.h's enum, it can be handled here.
  default: return "Error";
  }
}

void DoLogViewport(int j, const XF::Viewport& v) // Parameter changed to const XF::Viewport&
{
  // Stubbed for now, was HACK_LOG in VR-Hydra
  // HACK_LOG(VR, "  Viewport %d: %s (%g,%g) %gx%g; near=%g (%g), far=%g (%g)", j,
  //          GetViewportTypeName(g_viewport_type), v.xOrig - v.wd - 342, v.yOrig + v.ht - 342,
  //          2 * v.wd, -2 * v.ht, (v.farZ - v.zRange) / 16777216.0f, v.farZ - v.zRange,
  //          v.farZ / 16777216.0f, v.farZ);
}

void DoLogProj(int j, const float p[], const char* s) // Parameter changed to const float p[]
{
  // Stubbed, was HACK_LOG. Relies on g_ActiveConfig.iSelectedLayer
  // if (j == g_ActiveConfig.iSelectedLayer) HACK_LOG(VR, "** SELECTED LAYER:");
  // ... rest of logging logic ...
}

void LogProj(const float p[]) // Parameter changed to const float p[]
{
  // Stubbed. Depends on MetroidVR.h and debug_projNum, debug_newScene, etc.
  // Also vr_widest_3d_HFOV, etc.
}

void LogViewport(const XF::Viewport& v) // Parameter changed to const XF::Viewport&
{
  // Stubbed. Depends on debug_viewportNum, debug_newScene, etc.
}

void SetViewportType(const XF::Viewport& v) // Parameter changed to const XF::Viewport&
{
  g_old_viewport_type = g_viewport_type;
  float left, top, width, height;
  // GX hardware adds 342 to viewport coordinates.
  // xfmem.viewport.xOrig and yOrig are center points. wd and ht are half-width/height.
  // Screen space: (0,0) is top-left.
  left = v.xOrig - v.wd;
  top = v.yOrig - v.ht; // In GX, positive Y is down for viewport origin, but height is positive.
                        // For screen coordinates (0,0 top-left), top is yOrig - ht.
  width = 2.0f * v.wd;
  height = 2.0f * v.ht;

  // g_final_screen_region should be based on current EFB dimensions
  const float screen_width = static_cast<float>(g_framebuffer_manager->GetEFBWidth());
  const float screen_height = static_cast<float>(g_framebuffer_manager->GetEFBHeight());

  float min_screen_width = 0.90f * screen_width;
  float min_screen_height = 0.90f * screen_height;
  float max_top_offset = screen_height - min_screen_height; // Max deviation from top (0)
  float max_left_offset = screen_width - min_screen_width; // Max deviation from left (0)

  if (width == height && (width == 1 || width == 2 || width == 4 || ((int)width % 8 == 0)) &&
      (left == 0 || top == 0 || std::abs(top - (screen_height - height)) < 1.0f || std::abs(left - (screen_width - width)) < 1.0f ) &&
      !(width == 512 && screen_width == 512 && screen_height == 512))
  {
    g_viewport_type = ViewportType::RenderToTexture;
  }
  else if (width >= min_screen_width && std::abs(left) <= max_left_offset)
  {
    if (height >= min_screen_height && std::abs(top) <= max_top_offset)
    {
        g_viewport_type = (width == screen_width && height == screen_height && left == 0 && top == 0) ?
                           ViewportType::Fullscreen : ViewportType::Letterboxed;
    }
    // Simplified split-screen logic for now. More detailed checks from Hydra can be added if needed.
    else if (height >= min_screen_height / 2.1f && height <= screen_height / 1.9f)
    {
        if (std::abs(top) <= max_top_offset) g_viewport_type = ViewportType::Player1; // Top half
        else if (std::abs(top - height) <= max_top_offset) g_viewport_type = ViewportType::Player2; // Bottom half
        else g_viewport_type = ViewportType::Letterboxed;
    }
    else
    {
      g_viewport_type = ViewportType::Letterboxed;
    }
  }
  else if (height >= min_screen_height && std::abs(top) <= max_top_offset)
  {
     if (width >= min_screen_width / 2.1f && width <= screen_width / 1.9f)
     {
        if (std::abs(left) <= max_left_offset) g_viewport_type = ViewportType::Player1; // Left half
        else if (std::abs(left - width) <= max_left_offset) g_viewport_type = ViewportType::Player2; // Right half
        else g_viewport_type = ViewportType::HudElement;
     }
     else
     {
        g_viewport_type = ViewportType::Letterboxed;
     }
  }
  // Simplified quadrant logic
  else if (width >= (min_screen_width / 2.1f) && width <= (screen_width / 1.9f) &&
           height >= (min_screen_height / 2.1f) && height <= (screen_height / 1.9f))
  {
      if(std::abs(left) <= max_left_offset && std::abs(top) <= max_top_offset) g_viewport_type = ViewportType::Player1;
      else if(std::abs(left-width) <= max_left_offset && std::abs(top) <= max_top_offset) g_viewport_type = ViewportType::Player2;
      else if(std::abs(left) <= max_left_offset && std::abs(top-height) <= max_top_offset) g_viewport_type = ViewportType::Player3;
      else if(std::abs(left-width) <= max_left_offset && std::abs(top-height) <= max_top_offset) g_viewport_type = ViewportType::Player4;
      else g_viewport_type = ViewportType::HudElement;
  }
  else if (left >= screen_width || top >= screen_height || left + width <= 0 || top + height <= 0)
  {
    g_viewport_type = ViewportType::Offscreen;
  }
  else
  {
    g_viewport_type = ViewportType::HudElement;
  }

  if (g_viewport_type == ViewportType::Fullscreen || g_viewport_type == ViewportType::Letterboxed ||
      (g_viewport_type >= ViewportType::Player1 && g_viewport_type <= ViewportType::Player4))
  {
    float znear_norm = (v.farZ - v.zRange) / 16777216.0f;
    float zfar_norm = v.farZ / 16777216.0f;
    g_is_skybox = (znear_norm >= 0.99f && zfar_norm >= 0.999f);
  }
  else
  {
    g_is_skybox = false;
  }
}

void CheckOrientationConstants()
{
  // Stubbed. This function in VR-Hydra read game memory to determine camera orientation.
  // It's highly game-specific and complex. For a general VR port, this might be
  // replaced by a more generic solution or game profiles.
  // For now, it will do nothing, g_game_camera_pos/rotmat remain default.
  // If g_ActiveConfig.bCanReadCameraAngles is true, this is where the logic would go.
}

void CheckSkybox()
{
  if (xfmem.projection.type == ProjectionType::Perspective)
  {
    // In VR-Hydra, this checked constants.posnormalmatrix.
    // We need to ensure that posnormalmatrix is up-to-date before this check.
    // This check is very specific to how skyboxes are rendered (often at origin with identity model matrix).
    const float* p = VertexShaderManager::constants.posnormalmatrix[0].data(); // Assuming constants is updated
    if (p[3] == 0.0f && p[7] == 0.0f && p[11] == 0.0f) // Position part of matrix is zero (at origin)
    {
      // Check if it's not an identity matrix (which might be a UI element drawn at origin)
      if (p[0] != 1.0f || p[5] != 1.0f || p[10] != 1.0f)
      {
        g_is_skybox = true;
      }
    }
  }
}

void LockSkybox()
{
  if (xfmem.projection.type == ProjectionType::Perspective && g_is_skybox)
  {
    // If we have a previously locked skybox matrix, use it.
    // Otherwise, store the current one.
    // This assumes constants.posnormalmatrix is correctly set for the current skybox.
    if (s_had_skybox)
    {
      // Apply s_locked_skybox to current drawing context if needed.
      // This might involve directly setting XF registers or modifying draw state.
      // For now, this implies that if a skybox is detected and locked, subsequent skybox draws
      // use the locked matrix. The actual application of this locked matrix needs to be
      // integrated into the matrix setup for drawing.
      // For simplicity in this pass, we'll just note it. The main effect is that g_is_skybox
      // remains true and the original skybox matrix is preserved in s_locked_skybox.
      // The shader will use the posnormalmatrix from constants, which should be updated
      // with s_locked_skybox if this logic is to be fully effective.
      // memcpy(VertexShaderManager::constants.posnormalmatrix[0].data(), s_locked_skybox, sizeof(s_locked_skybox));
    }
    else
    {
      memcpy(s_locked_skybox, VertexShaderManager::constants.posnormalmatrix[0].data(), sizeof(s_locked_skybox));
      s_had_skybox = true;
    }
  }
}
// --- End of VR-Hydra helper functions ---


// VR-Hydra global static variables (some might be adapted/removed)
static float GC_ALIGNED16(g_fProjectionMatrix[16]); // Used by VR-Hydra's SetProjectionConstants and helpers
static float s_locked_skybox[3 * 4];
static bool s_had_skybox = false;

// track changes (many of these might be replaced by XFStateManager)
// static bool bTexMatricesChanged[2], bPosNormalMatrixChanged, bProjectionChanged, bViewportChanged;
// static bool bFreeLookChanged, bFrameChanged;
// static bool bTexMtxInfoChanged, bLightingConfigChanged;
// static Common::BitSet32 nMaterialsChanged;
// static int nTransformMatricesChanged[2];
// static int nNormalMatricesChanged[2];
// static int nPostTransformMatricesChanged[2];
// static int nLightsChanged[2];

// Viewport related globals from VR-Hydra
EFBRectangle g_final_screen_region = EFBRectangle(0, 0, EFB_WIDTH, EFB_HEIGHT); // Default, might be updated
EFBRectangle g_requested_viewport = EFBRectangle(0, 0, EFB_WIDTH, EFB_HEIGHT); // Default
EFBRectangle g_rendered_viewport = EFBRectangle(0, 0, EFB_WIDTH, EFB_HEIGHT); // Default
ViewportType g_viewport_type = ViewportType::Fullscreen; // Default enum value
ViewportType g_old_viewport_type = ViewportType::Fullscreen; // Default enum value
SplitScreenType g_splitscreen_type = SplitScreenType::Fullscreen; // Default enum value
SplitScreenType g_old_splitscreen_type = SplitScreenType::Fullscreen; // Default enum value
bool g_is_skybox = false;
Common::Matrix44 g_game_camera_rotmat = Common::Matrix44::Identity();
float g_game_camera_pos[3] = {0.0f, 0.0f, 0.0f};
bool g_vr_had_3D_already = false; // From VR.cpp in Hydra, seems relevant here

// Debugging vars from VR-Hydra (might be ifdef'd out later)
int vr_render_eye = -1; // from VR.h
// int debug_viewportNum = 0;
// Viewport debug_vpList[64] = {0}; // Viewport struct needs to be defined or adapted
// int debug_projNum = 0;
// float debug_projList[64][7] = {0};
// bool debug_newScene = true, debug_nextScene = false; // from VR.cpp
float vr_widest_3d_HFOV = 0; // from VR.cpp
float vr_widest_3d_VFOV = 0; // from VR.cpp
float vr_widest_3d_zNear = 0; // from VR.cpp
float vr_widest_3d_zFar = 0; // from VR.cpp
int vr_widest_3d_projNum = -1; // from VR.cpp

// Metroid Prime specific layer info (example of game-specific VR data)
MetroidLayer g_metroid_layer = MetroidLayer::METROID_UNKNOWN; // from MetroidVR.h
bool g_is_morph_ball = false; // from MetroidVR.h
bool g_is_first_person_visor_active = false; // from MetroidVR.h
bool g_is_nes = false; // Example, if needed for NES VC games

// Define static members
VertexShaderConstants VertexShaderManager::constants;
float4 VertexShaderManager::constants_eye_projection[2][4];
bool VertexShaderManager::m_layer_on_top = false;
bool VertexShaderManager::dirty = true; // Initialize dirty to true

// VR view manipulation functions (operating on g_freelook_camera)
void VertexShaderManager::ScaleView(float scale)
{
  // This function in VR-Hydra scaled s_fViewTranslationVector.
  // For VR-Reloaded, we'd need to adjust g_freelook_camera's internal position or scale factor.
  // g_freelook_camera.Scale(scale); // Placeholder for actual FreeLookCamera API
  // For now, mark projection as dirty as view parameters change.
  // The actual scaling of world vs camera movement needs to be handled in SetConstants.
  g_freelook_camera.GetController()->SetDirty();
  VertexShaderManager::dirty = true;
}

void VertexShaderManager::TranslateView(float left_metres, float forward_metres, float down_metres)
{
  // VR-Hydra transformed these by s_viewInvRotationMatrix then added to s_fViewTranslationVector.
  // Here, we should call methods on g_freelook_camera.
  // Assuming g_freelook_camera has methods like MoveForward, MoveRight, MoveUp.
  // The coordinate system needs to match.
  // float move_speed = FreeLook::GetActiveConfig().camera_config.move_speed; // Might be useful

  // This is a simplified adaptation. The actual FreeLookCamera API would be used.
  // These translations are in camera space.
  Common::Vec3 translation_cam_space = {left_metres, -down_metres, -forward_metres}; // often -forward
  g_freelook_camera.GetController()->Translate(translation_cam_space);
  VertexShaderManager::dirty = true;
}

void VertexShaderManager::RotateView(float x_rad, float y_rad)
{
  // VR-Hydra updated s_fViewRotation and recalculated s_viewRotationMatrix.
  // Here, we call g_freelook_camera's rotation methods.
  g_freelook_camera.GetController()->Turn(x_rad, y_rad); // Assuming Turn takes radians
  VertexShaderManager::dirty = true;
}

void VertexShaderManager::ResetView()
{
  g_freelook_camera.Reset();
  // VR-Hydra also reset its specific VRTracker::ResetView();
  // If VRTracker is part of VR.h or a separate system, its reset should be called too.
  // For now, just reset freelook.
  VertexShaderManager::dirty = true;
}


void VertexShaderManager::Init()
{
  // Initialize state tracking variables
  m_projection_graphics_mod_change = false;

  constants = {};
  // Initialize new static VR members
  memset(constants_eye_projection, 0, sizeof(constants_eye_projection));
  m_layer_on_top = false;


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

void VertexShaderManager::SetProjectionMatrix(XFStateManager& xf_state_manager)
{
  if (xf_state_manager.DidProjectionChange() || g_freelook_camera.GetController()->IsDirty())
  {
    xf_state_manager.ResetProjection();
    auto corrected_matrix = LoadProjectionMatrix();
    memcpy(constants.projection.data(), corrected_matrix.data.data(), 4 * sizeof(float4));
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
  // --- Start of VR-Hydra SetProjectionConstants merge ---
  // static bool temp_skybox = false; // Will be handled by g_is_skybox directly
  bool position_changed = xf_state_manager.DidPosNormalChange() || xf_state_manager.GetPerVertexTransformMatrixChanges()[0] >=0; // Approximate
  bool skybox_changed = false; // Will be set if g_is_skybox changes or viewport type changes

  // First, identify any special layers and hacks
  VertexShaderManager::m_layer_on_top = false; // Use the static member
  bool bFullscreenLayer = false; // Will be determined by viewport type and VR config
  // bool bFlashing = (debug_projNum - 1) == g_ActiveConfig.iSelectedLayer; // Debug, skip for now
  bool bFlashing = false; // Stub
  bool bStuckToHead = false, bHide = false;
  int flipped_x = 1, flipped_y = 1, iTelescopeHack = -1;
  float fScaleHack = 1.0f, fWidthHack = 1.0f, fHeightHack = 1.0f, fUpHack = 0.0f, fRightHack = 0.0f;

  // TODO: Port GetMetroidPrimeValues or similar game-specific VR logic mechanism
  // if (g_ActiveConfig.iMetroidPrime || g_is_nes)
  // {
  //   GetMetroidPrimeValues(&bStuckToHead, &bFullscreenLayer, &bHide, &bFlashing, &fScaleHack,
  //                         &fWidthHack, &fHeightHack, &fUpHack, &fRightHack, &iTelescopeHack);
  // }

  // VR: in split-screen, only draw VR player. TODO: fix offscreen to render to a separate texture in VR
  // g_has_hmd will come from VR.h
  bHide = bHide ||
          (g_has_hmd && (g_viewport_type == ViewportType::Offscreen ||
                         (g_viewport_type >= ViewportType::Player1 && g_viewport_type <= ViewportType::Player4 &&
                          g_ActiveConfig.iVRPlayer != static_cast<int>(g_viewport_type) - static_cast<int>(ViewportType::Player1))));
  // flash selected layer for debugging - skipping for now
  // bHide = bHide || (bFlashing && g_ActiveConfig.iFlashState > 5);
  // hide skybox or everything to reduce motion sickness
  // g_vr_black_screen will come from VR.h
  bHide = bHide || (g_is_skybox && g_ActiveConfig.iMotionSicknessSkybox == 1) || g_vr_black_screen;

  // Telescope Hack
  float fLeftWidthHack = fWidthHack, fRightWidthHack = fWidthHack;
  float fLeftHeightHack = fHeightHack, fRightHeightHack = fHeightHack;
  bool bHideLeft = bHide, bHideRight = bHide, bNoForward = false;

  if (iTelescopeHack < 0 && g_ActiveConfig.iTelescopeEye != 0 && // Assuming 0 means no telescope eye
      vr_widest_3d_VFOV <= g_ActiveConfig.fTelescopeMaxFOV && vr_widest_3d_VFOV > 1.0f &&
      (g_ActiveConfig.fTelescopeMaxFOV <= g_ActiveConfig.fMinFOV ||
       (g_ActiveConfig.fTelescopeMaxFOV > g_ActiveConfig.fMinFOV &&
        vr_widest_3d_VFOV > g_ActiveConfig.fMinFOV)))
  {
    iTelescopeHack = g_ActiveConfig.iTelescopeEye;
  }

  if (g_has_hmd && iTelescopeHack > 0)
  {
    bNoForward = true;
    float hmd_halftan_v; // Vertical half tanFOV
    // TODO: VR_GetProjectionHalfTan(&hmd_halftan_v, nullptr); // Assuming this function provides vertical half tan FOV
    // For now, approximate or assume a default if VR_GetProjectionHalfTan is not yet available
    hmd_halftan_v = tanf(DEGREES_TO_RADIANS(90.0f) / 2.0f); // Placeholder for HMD's actual V FOV

    float telescope_scale = fabsf(hmd_halftan_v / tanf(DEGREES_TO_RADIANS(vr_widest_3d_VFOV) / 2.0f));
    if (iTelescopeHack & 1) // Left eye
    {
      fLeftWidthHack *= telescope_scale;
      fLeftHeightHack *= telescope_scale;
      bHideLeft = false;
    }
    if (iTelescopeHack & 2) // Right eye
    {
      fRightWidthHack *= telescope_scale;
      fRightHeightHack *= telescope_scale;
      bHideRight = false;
    }
  }
  // --- End of initial VR-Hydra variable setup ---

  // This part replaces the call to SetProjectionMatrix and integrates its logic
  // It also starts the main VR transformation pipeline logic from VR-Hydra's SetProjectionConstants
  if (xf_state_manager.DidProjectionChange() || g_freelook_camera.GetController()->IsDirty() ||
      !projection_actions.empty() || m_projection_graphics_mod_change || VertexShaderManager::dirty) // Added VSM::dirty for VR changes
  {
    xf_state_manager.ResetProjection(); // Still call this from XFStateManager
    // m_projection_graphics_mod_change = !projection_actions.empty(); // Already handled below

    // Load raw projection into g_fProjectionMatrix (from VR-Hydra)
    // This is similar to what VR-Reloaded-4's LoadProjectionMatrix started with
    const float* rawProjection = xfmem.projection.rawProjection;
    switch (xfmem.projection.type)
    {
    case ProjectionType::Perspective:
      g_fProjectionMatrix[0] = rawProjection[0]; // Aspect ratio hacks will be applied later if needed or by VR path
      g_fProjectionMatrix[1] = 0.0f;
      g_fProjectionMatrix[2] = rawProjection[1];
      g_fProjectionMatrix[3] = 0.0f;
      g_fProjectionMatrix[4] = 0.0f;
      g_fProjectionMatrix[5] = rawProjection[2];
      g_fProjectionMatrix[6] = rawProjection[3];
      g_fProjectionMatrix[7] = 0.0f;
      g_fProjectionMatrix[8] = 0.0f;
      g_fProjectionMatrix[9] = 0.0f;
      g_fProjectionMatrix[10] = rawProjection[4];
      g_fProjectionMatrix[11] = rawProjection[5];
      g_fProjectionMatrix[12] = 0.0f;
      g_fProjectionMatrix[13] = 0.0f;
      g_fProjectionMatrix[14] = -1.0f;
      g_fProjectionMatrix[15] = 0.0f;
      // SETSTAT_FT for gproj can be added if needed, or handled by existing stats system
      break;
    case ProjectionType::Orthographic:
      g_fProjectionMatrix[0] = rawProjection[0];
      g_fProjectionMatrix[1] = 0.0f;
      g_fProjectionMatrix[2] = 0.0f;
      g_fProjectionMatrix[3] = rawProjection[1];
      g_fProjectionMatrix[4] = 0.0f;
      g_fProjectionMatrix[5] = rawProjection[2];
      g_fProjectionMatrix[6] = 0.0f;
      g_fProjectionMatrix[7] = rawProjection[3];
      g_fProjectionMatrix[8] = 0.0f;
      g_fProjectionMatrix[9] = 0.0f;
      // TODO: Integrate projection hack logic from VR-Hydra if still desired
      // g_fProjectionMatrix[10] = (g_proj_hack_near.value + rawProjection[4]) * ((g_proj_hack_near.sign == 0) ? 1.0f : g_proj_hack_near.sign);
      // g_fProjectionMatrix[11] = (g_proj_hack_far.value + rawProjection[5]) * ((g_proj_hack_far.sign == 0) ? 1.0f : g_proj_hack_far.sign);
      g_fProjectionMatrix[10] = rawProjection[4]; // Simplified for now
      g_fProjectionMatrix[11] = rawProjection[5];
      g_fProjectionMatrix[12] = 0.0f;
      g_fProjectionMatrix[13] = 0.0f;
      g_fProjectionMatrix[14] = 0.0f;
      g_fProjectionMatrix[15] = 1.0f;
      // SETSTAT_FT for g2proj can be added
      break;
    default:
      ERROR_LOG(VIDEO, "Unknown projection type: %d", xfmem.projection.type);
    }
    // PRIM_LOG("Projection: %f %f %f %f %f %f", rawProjection[0], rawProjection[1], rawProjection[2], rawProjection[3], rawProjection[4], rawProjection[5]);
    // LogProj(xfmem.projection.rawProjection); // TODO: Port LogProj and its dependencies

    VertexShaderManager::dirty = true; // Mark constants dirty as projection changed
    GeometryShaderManager::dirty = true; // GS also depends on projection params often

    bool bN64 = (xfmem.projection.type == ProjectionType::Perspective && rawProjection[0] == 1.0f &&
                 rawProjection[1] == 1.0f && rawProjection[2] == 1.0f && rawProjection[3] == 1.0f);

    float UnitsPerMetre = g_ActiveConfig.fUnitsPerMetre * fScaleHack / g_ActiveConfig.fScale;

    // bHide was calculated earlier
    if (bHide)
    {
      memset(constants.projection.data(), 0, sizeof(constants.projection));
      memset(constants_eye_projection, 0, sizeof(constants_eye_projection));
      memset(GeometryShaderManager::constants.stereoparams.data(), 0, sizeof(GeometryShaderManager::constants.stereoparams));
    }
    else if (g_viewport_type == ViewportType::RenderToTexture) // TODO: Ensure g_viewport_type is correctly set
    {
      Common::Matrix44 correctedMtx = Common::Matrix44::FromArray(g_fProjectionMatrix);
      // VR-Hydra applied s_viewportCorrection here if !g_ActiveConfig.backend_info.bSupportsOversizedViewports
      // VR-Reloaded-4's SetScissorAndViewport handles viewport scaling. For RTT, direct matrix is usually fine.
      memcpy(constants.projection.data(), correctedMtx.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[0], correctedMtx.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[1], correctedMtx.data.data(), sizeof(float4) * 4);
      GeometryShaderManager::constants.stereoparams[0] = 0.0f;
      GeometryShaderManager::constants.stereoparams[1] = 0.0f;
      GeometryShaderManager::constants.stereoparams[2] = 0.0f;
      GeometryShaderManager::constants.stereoparams[3] = 0.0f;
    }
    else if (!g_has_hmd || !g_ActiveConfig.bEnableVR) // Standard path (No VR HMD or VR disabled)
    {
      Common::Matrix44 corrected_matrix = Common::Matrix44::FromArray(g_fProjectionMatrix);
      // Apply aspect ratio hack similar to VR-Reloaded-4's LoadProjectionMatrix
      if (xfmem.projection.type == ProjectionType::Perspective) {
          corrected_matrix.data[0] *= g_ActiveConfig.fAspectRatioHackW;
          corrected_matrix.data[2] *= g_ActiveConfig.fAspectRatioHackW;
          corrected_matrix.data[5] *= g_ActiveConfig.fAspectRatioHackH;
          corrected_matrix.data[6] *= g_ActiveConfig.fAspectRatioHackH;
      }

      if (g_freelook_camera.IsActive() && xfmem.projection.type == ProjectionType::Perspective)
      {
        // Apply FreeLook camera transformations
        // The VR-Hydra logic multiplied projection * view. Here, view is from g_freelook_camera.
        // s_viewportCorrection was also applied in VR-Hydra.
        // VR-Reloaded-4's SetScissorAndViewport should handle viewport scaling correctly.
        corrected_matrix *= g_freelook_camera.GetView();
      }
      g_freelook_camera.GetController()->SetClean(); // Matches VR-Reloaded-4 LoadProjectionMatrix

      memcpy(constants.projection.data(), corrected_matrix.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[0], corrected_matrix.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[1], corrected_matrix.data.data(), sizeof(float4) * 4);

      if (g_ActiveConfig.stereo_mode != StereoMode::Off) // Standard stereo
      {
        if (xfmem.projection.type == ProjectionType::Perspective)
        {
          float offset = (g_ActiveConfig.iStereoDepth / 1000.0f) * (g_ActiveConfig.iStereoDepthPercentage / 100.0f);
          GeometryShaderManager::constants.stereoparams[0] = (g_ActiveConfig.bStereoSwapEyes) ? offset : -offset;
          GeometryShaderManager::constants.stereoparams[1] = (g_ActiveConfig.bStereoSwapEyes) ? -offset : offset;
        }
        else
        {
          GeometryShaderManager::constants.stereoparams[0] = 0.0f;
          GeometryShaderManager::constants.stereoparams[1] = 0.0f;
        }
        GeometryShaderManager::constants.stereoparams[2] = (float)(g_ActiveConfig.iStereoConvergence * (g_ActiveConfig.iStereoConvergencePercentage / 100.0f));
        GeometryShaderManager::constants.stereoparams[3] = 0.0f; // VR Hydra used stereoparams[2] for convergence, [3] was Oculus offaxis. For standard stereo, this is likely unused.
      }
      else // Mono
      {
        GeometryShaderManager::constants.stereoparams[0] = 0.0f;
        GeometryShaderManager::constants.stereoparams[1] = 0.0f;
        GeometryShaderManager::constants.stereoparams[2] = 0.0f;
        GeometryShaderManager::constants.stereoparams[3] = 0.0f;
      }
    }
    else if (bFullscreenLayer) // VR Fullscreen layer (e.g. EFB copies)
    {
      Common::Matrix44 projMtx = Common::Matrix44::FromArray(g_fProjectionMatrix);
      projMtx.data[0] *= fWidthHack; // These hacks come from GetMetroidPrimeValues or similar
      projMtx.data[5] *= fHeightHack;
      projMtx.data[3] += fRightHack; // wx in Hydra was projMtx.data[3] or [12] depending on row/col major
      projMtx.data[7] += fUpHack;    // wy in Hydra was projMtx.data[7] or [13]

      memcpy(constants.projection.data(), projMtx.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[0], projMtx.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[1], projMtx.data.data(), sizeof(float4) * 4);
      GeometryShaderManager::constants.stereoparams[0] = 0.0f;
      GeometryShaderManager::constants.stereoparams[1] = 0.0f;
      GeometryShaderManager::constants.stereoparams[2] = 0.0f;
      GeometryShaderManager::constants.stereoparams[3] = 0.0f;
    }
    else // Main VR 3D HMD path
    {
      // This is the most complex part, involving full VR transformation pipeline.
      // --- Start of Main VR 3D HMD Path ---
      float znear, zfar, hfov, vfov; // Game's FOV and clip distances

      // if the camera is zoomed in so much that the action only fills a tiny part of your FOV,
      // we need to move the camera forwards until objects at AimDistance fill the minimum FOV.
      float zoom_forward = 0.0f;
      if (vr_widest_3d_HFOV <= g_ActiveConfig.fMinFOV && vr_widest_3d_HFOV > 0.0f && iTelescopeHack <= 0)
      {
        zoom_forward = g_ActiveConfig.fAimDistance *
                       tanf(DEGREES_TO_RADIANS(g_ActiveConfig.fMinFOV) / 2.0f) /
                       tanf(DEGREES_TO_RADIANS(vr_widest_3d_HFOV) / 2.0f);
        zoom_forward -= g_ActiveConfig.fAimDistance;
      }

      if (xfmem.projection.type == ProjectionType::Perspective && g_viewport_type != ViewportType::HudElement && g_viewport_type != ViewportType::Offscreen)
      {
        // TODO: Port CheckOrientationConstants(); - This sets g_game_camera_pos and g_game_camera_rotmat
        // For now, assume they are identity/zero.
        // CheckOrientationConstants();
        g_vr_had_3D_already = true;

        // Calculate game's clipping planes and FOV from its projection matrix (g_fProjectionMatrix)
        // Assuming g_fProjectionMatrix is column-major as in original Dolphin:
        // P = | A C Px 0 |
        //     | 0 B Py 0 |
        //     | 0 0 Q  R |
        //     | 0 0 -1 0 | (for perspective)
        // Q = (Far+Near)/(Near-Far) or Far/(Far-Near) depending on convention
        // R = (2*Far*Near)/(Near-Far) or (-Far*Near)/(Far-Near)
        // A = 1/tan(HFOV/2), B = 1/tan(VFOV/2) (if Px, Py are 0)
        // For simplicity, using VR-Hydra's direct calculation from rawProjection values
        // (rawProjection maps to xfmem.projection.rawProjection)
        const float* p = xfmem.projection.rawProjection; // p[0]=A, p[1]=Px, p[2]=B, p[3]=Py, p[4]=Q, p[5]=R (relative to GX proj)

        // These calculations are based on g_fProjectionMatrix which has already processed rawProjection
        // For a standard perspective matrix (Px=0, Py=0, Pz=-1, Pw=0 in last column of view matrix):
        // Q = g_fProjectionMatrix[10], R = g_fProjectionMatrix[11]
        // if g_fProjectionMatrix[14] == -1 (typical perspective):
        // Near = R / (Q - 1)
        // Far  = R / (Q + 1)
        // OR, if Q = far/(far-near) and R = -far*near/(far-near) [D3D style]
        // znear = -R/Q; zfar = R/(1-Q);
        // OR, if Q = (f+n)/(n-f) and R = (2fn)/(n-f) [OpenGL style, what Dolphin seems to use]
        // znear = R / (Q-1); zfar = R / (Q+1);
        // Let's use the Hydra calculation based on raw values for now as it's tested for Dolphin 5.0
        // zfar = p[5] / p[4]; znear = (1 + p[5]) / p[4]; // This seems off, p[4] can be near 0
        // This is more standard:
        if ( (p[4]-1.0f) != 0.0f && (p[4]+1.0f) != 0.0f) {
             znear = p[5] / (p[4] - 1.0f);
             zfar  = p[5] / (p[4] + 1.0f);
        } else { // Fallback for orthographic-like perspective or problematic values
             znear = 0.1f * UnitsPerMetre; // Default near
             zfar = 1000.0f * UnitsPerMetre; // Default far
        }
        // Ensure near < far and both positive
        if (znear >= zfar || znear <= 0.0f) {
            znear = 0.1f * UnitsPerMetre;
            if (zfar <= znear) zfar = znear + 100.0f * UnitsPerMetre;
        }

        // hfov = 2.0f * atanf(1.0f / g_fProjectionMatrix[0]) * RADIANS_TO_DEGREES;
        // vfov = 2.0f * atanf(1.0f / g_fProjectionMatrix[5]) * RADIANS_TO_DEGREES;
        // Using rawProjection based FOV from Hydra for consistency:
        hfov = 2.0f * atanf(1.0f / p[0]) * RADIANS_TO_DEGREES;
        vfov = 2.0f * atanf(1.0f / p[2]) * RADIANS_TO_DEGREES;
      }
      else // 2D layer or 3D HUD element that we treat as 2D initially
      {
        m_layer_on_top = g_ActiveConfig.bHudOnTop;
        if (vr_widest_3d_HFOV > 0.0f) // If we have seen a 3D projection this frame
        {
          znear = vr_widest_3d_zNear;
          zfar = vr_widest_3d_zFar;
          hfov = (zoom_forward != 0.0f) ? g_ActiveConfig.fMinFOV : vr_widest_3d_HFOV;
          vfov = hfov * (vr_widest_3d_VFOV / vr_widest_3d_HFOV); // Maintain aspect of widest 3D
        }
        else // Default if no 3D in scene
        {
          znear = 0.2f * UnitsPerMetre * 20.0f; // 50cm (Hydra had typo, 0.2*U*20 != 50cm) -> should be 0.5f * UnitsPerMetre
          zfar = 40.0f * UnitsPerMetre;
          hfov = 70.0f;
          // TODO: g_renderer will be replaced by g_gfx
          // vfov = g_renderer->m_aspect_wide ? hfov * (9.0f/16.0f) : hfov * (3.0f/4.0f);
          vfov = g_Config.iAspectRatio == static_cast<int>(AspectMode::ForceWide) ? hfov * (9.0f/16.0f) : hfov * (3.0f/4.0f); // Approximation
        }
        // For 2D layers that are transformed to 3D, use a temporary, thin projection
        // The actual scaling and positioning happens in the modelview matrix for these.
        // g_fProjectionMatrix is set to a basic perspective using these derived FOV/clip for 2D->3D transform.
        // This is a bit simplified from Hydra, actual matrix construction for HUD comes later.
        Common::Matrix44 basic_persp;
        BuildPerspectiveMat(basic_persp.data, hfov, vfov, znear / 40.0f, zfar); // very thin near for HUD
        memcpy(g_fProjectionMatrix, basic_persp.data.data(), sizeof(g_fProjectionMatrix));
      }

      Common::Matrix44 proj_left_hmd, proj_right_hmd;
      // TODO: Port VR_GetProjectionMatrices(proj_left_hmd, proj_right_hmd, znear, zfar);
      // For now, use a placeholder. This function is critical.
      BuildPerspectiveMat(proj_left_hmd.data, 90.0f, 90.0f, znear, zfar); // Placeholder HMD projection
      proj_right_hmd = proj_left_hmd; // Placeholder

      // Apply VR hacks to HMD projection (width, height, up, right adjustments)
      // This part was complex in Hydra, involving fLeftWidthHack etc.
      // Simplified: assume these hacks modify proj_left_hmd and proj_right_hmd directly if needed.
      // Example: proj_left_hmd.data[0] *= fLeftWidthHack; proj_left_hmd.data[5] *= fLeftHeightHack; etc.

      GeometryShaderManager::constants.stereoparams[0] = proj_left_hmd.data[0];  // projection[0][0] for left
      GeometryShaderManager::constants.stereoparams[1] = proj_right_hmd.data[0]; // projection[0][0] for right
      GeometryShaderManager::constants.stereoparams[2] = proj_left_hmd.data[2];  // projection[0][2] (off-axis x) for left
      GeometryShaderManager::constants.stereoparams[3] = proj_right_hmd.data[2]; // projection[0][2] (off-axis x) for right

      // Transformation Chain:
      Common::Matrix44 mat_camera_stabilize = Common::Matrix44::Identity();
      Common::Matrix44 mat_camera_forward = Common::Matrix44::Identity();
      Common::Matrix44 mat_camera_pitch = Common::Matrix44::Identity();
      Common::Matrix44 mat_free_look = g_freelook_camera.IsActive() ? g_freelook_camera.GetView() : Common::Matrix44::Identity();
      Common::Matrix44 mat_lean_back = Common::Matrix44::Identity();
      Common::Matrix44 mat_head_pos = Common::Matrix44::Identity();
      Common::Matrix44 mat_head_rot = Common::Matrix44::Identity(); // From g_head_tracking_matrix
      Common::Matrix44 mat_eye_offset_left = Common::Matrix44::Identity();
      Common::Matrix44 mat_eye_offset_right = Common::Matrix44::Identity();

      // TODO: Populate these matrices based on ported logic and VR SDK data
      // Example for camera stabilization (uses g_game_camera_pos, g_game_camera_rotmat)
      // mat_camera_stabilize = g_game_camera_rotmat.Inverse() * Common::Matrix44::Translate(-g_game_camera_pos[0], -g_game_camera_pos[1], -g_game_camera_pos[2]);

      // Example for camera forward
      // if (!bNoForward && !g_is_skybox && !bStuckToHead) {
      //    mat_camera_forward = Common::Matrix44::Translate(0,0, (g_ActiveConfig.fCameraForward + zoom_forward) * UnitsPerMetre);
      // }

      // Final view matrices for each eye
      Common::Matrix44 view_matrix_left = mat_eye_offset_left * mat_head_rot * mat_head_pos * mat_lean_back * mat_free_look * mat_camera_pitch * mat_camera_forward * mat_camera_stabilize;
      Common::Matrix44 view_matrix_right = mat_eye_offset_right * mat_head_rot * mat_head_pos * mat_lean_back * mat_free_look * mat_camera_pitch * mat_camera_forward * mat_camera_stabilize;

      // Apply game's original model view (from g_fProjectionMatrix if it was actually modelview, or identity if g_fProjectionMatrix was pure projection)
      // This part is tricky. VR-Hydra's g_fProjectionMatrix was often the game's raw projection.
      // The "look_matrix" in Hydra combined game's model transform with VR transforms.
      // Here, we assume game's modelview is implicitly applied before shader, or is part of `constants.posnormalmatrix` etc.
      // So, the view_matrix_left/right are camera transforms.
      // The final projection for the shader is HMD_Proj * Eye_Offset * Head_Rot * Head_Pos * ... * GameWorldToView
      // This means view_matrix_left/right are effectively WorldToEye matrices.
      // The shader then does: FinalClip = HMD_Proj * WorldToEye * ModelToWorld * VertexPos
      // Our `constants.projection` should be HMD_Proj.
      // The `constants.posnormalmatrix` etc. handle ModelToWorld.
      // The part that transforms from World to Eye (view_matrix_left/right) needs to be passed differently or pre-multiplied.

      // VR-Hydra set constants.projection to final_matrix_left.
      // And constants_eye_projection[0] = final_matrix_left, constants_eye_projection[1] = final_matrix_right.
      // This implies that `constants.projection` was `HMD_Left_Proj * View_Left`.
      // Let's follow that.
      Common::Matrix44 final_left = proj_left_hmd * view_matrix_left;
      Common::Matrix44 final_right = proj_right_hmd * view_matrix_right;

      memcpy(constants.projection.data(), final_left.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[0], final_left.data.data(), sizeof(float4) * 4);
      memcpy(constants_eye_projection[1], final_right.data.data(), sizeof(float4) * 4);
      // --- End of Main VR 3D HMD Path ---
    }
    // Update graphics mod system if projection changed
    m_projection_graphics_mod_change = !projection_actions.empty();
  }


  if (constants.missing_color_hex != g_ActiveConfig.iMissingColorValue)
  {
    const float a = (g_ActiveConfig.iMissingColorValue) & 0xFF;
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
