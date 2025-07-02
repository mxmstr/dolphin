// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/Matrix.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VideoCommon.h" // For EFBRectangle

class PointerWrap;
struct PortableVertexDeclaration;
class XFStateManager;

// The non-API dependent parts.
// From VR-Hydra
enum ViewportType
{
  VIEW_FULLSCREEN = 0,
  VIEW_LETTERBOXED,
  VIEW_HUD_ELEMENT,
  VIEW_SKYBOX,
  VIEW_PLAYER_1,
  VIEW_PLAYER_2,
  VIEW_PLAYER_3,
  VIEW_PLAYER_4,
  VIEW_OFFSCREEN,
  VIEW_RENDER_TO_TEXTURE,
};

// These will be defined in VertexShaderManager.cpp
extern ViewportType g_viewport_type;
extern bool g_is_skybox;
extern EFBRectangle g_final_screen_region; // Used by some VR logic

class alignas(16) VertexShaderManager
{
public:
  void Init();
  void DoState(PointerWrap& p);

  // constant management
  void SetProjectionMatrix(XFStateManager& xf_state_manager);
  void SetConstants(const std::vector<std::string>& textures, XFStateManager& xf_state_manager);

  // data: 3 floats representing the X, Y and Z vertex model coordinates and the posmatrix index.
  // out:  4 floats which will be initialized with the corresponding clip space coordinates
  // NOTE: m_projection_matrix must be up to date when this is called
  //       (i.e. VertexShaderManager::SetConstants needs to be called before using this!)
  void TransformToClipSpace(const float* data, float* out, u32 mtxIdx);

  static bool UseVertexDepthRange();

  // VR Free look camera controls (adapted from Hydra)
  // These are static as they modify global view parameters or interact with FreeLookCamera
  static void TranslateView(float left_metres, float forward_metres, float down_metres = 0.0f);
  static void RotateView(float x_degrees, float y_degrees);
  static void ScaleView(float scale); // May not be needed if g_ActiveConfig.fScale is primary
  static void ResetView();

  // VR-Hydra had these as static members.
  // constants_eye_projection will store the final per-eye matrices.
  // The main 'constants.projection' can store the mono/left-eye for shared VS paths.
  static Common::Matrix44 constants_eye_projection[2];
  static float s_locked_skybox[12]; // 3x4 matrix
  static bool s_had_skybox;
  static Common::Matrix44 g_game_camera_rotmat;
  static float g_game_camera_pos[3];


  VertexShaderConstants constants{};
  bool dirty = false;

  static DOLPHIN_FORCE_INLINE void UpdateValue(bool* dirty, u32* old_value, u32 new_value)
  {
    if (*old_value == new_value)
      return;
    *old_value = new_value;
    *dirty = true;
  }

  static DOLPHIN_FORCE_INLINE void UpdateOffset(bool* dirty, bool include_components,
                                                u32* old_value, const AttributeFormat& attribute)
  {
    if (!attribute.enable)
      return;
    u32 new_value = attribute.offset / 4;  // GPU uses uint offsets
    if (include_components)
      new_value |= attribute.components << 16;
    UpdateValue(dirty, old_value, new_value);
  }

  template <size_t N>
  static DOLPHIN_FORCE_INLINE void UpdateOffsets(bool* dirty, bool include_components,
                                                 std::array<u32, N>* old_value,
                                                 const std::array<AttributeFormat, N>& attribute)
  {
    for (size_t i = 0; i < N; i++)
      UpdateOffset(dirty, include_components, &(*old_value)[i], attribute[i]);
  }

  DOLPHIN_FORCE_INLINE void SetVertexFormat(u32 components, const PortableVertexDeclaration& format)
  {
    UpdateValue(&dirty, &constants.components, components);
    UpdateValue(&dirty, &constants.vertex_stride, format.stride / 4);
    UpdateOffset(&dirty, true, &constants.vertex_offset_position, format.position);
    UpdateOffset(&dirty, false, &constants.vertex_offset_posmtx, format.posmtx);
    UpdateOffsets(&dirty, true, &constants.vertex_offset_texcoords, format.texcoords);
    UpdateOffsets(&dirty, false, &constants.vertex_offset_colors, format.colors);
    UpdateOffsets(&dirty, false, &constants.vertex_offset_normals, format.normals);
  }

private:
  alignas(16) std::array<float, 16> m_projection_matrix;

  // track changes
  bool m_projection_graphics_mod_change = false;

  Common::Matrix44 LoadProjectionMatrix();
};
