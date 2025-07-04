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
ViewportType g_viewport_type, g_old_viewport_type;

class PointerWrap;
struct PortableVertexDeclaration;
class XFStateManager;

// The non-API dependent parts.
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

  static VertexShaderConstants constants; // Made static for VR port access
  static float4 constants_eye_projection[2][4]; // For VR per-eye projection
  static bool m_layer_on_top; // For VR HUD handling
  static bool dirty; // Made static

  // VR FreeLook/View manipulation functions (will operate on g_freelook_camera)
  static void ScaleView(float scale);
  static void TranslateView(float left_metres, float forward_metres, float down_metres = 0.0f);
  static void RotateView(float x, float y);
  static void ResetView();

  static DOLPHIN_FORCE_INLINE void UpdateValue(bool* pDirty, u32* old_value, u32 new_value)
  {
    if (*old_value == new_value)
      return;
    *old_value = new_value;
    *pDirty = true;
  }

  static DOLPHIN_FORCE_INLINE void UpdateOffset(bool* pDirty, bool include_components,
                                                u32* old_value, const AttributeFormat& attribute)
  {
    if (!attribute.enable)
      return;
    u32 new_value = attribute.offset / 4;  // GPU uses uint offsets
    if (include_components)
      new_value |= attribute.components << 16;
    UpdateValue(pDirty, old_value, new_value);
  }

  template <size_t N>
  static DOLPHIN_FORCE_INLINE void UpdateOffsets(bool* pDirty, bool include_components,
                                                 std::array<u32, N>* old_value,
                                                 const std::array<AttributeFormat, N>& attribute)
  {
    for (size_t i = 0; i < N; i++)
      UpdateOffset(pDirty, include_components, &(*old_value)[i], attribute[i]);
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
  bool m_projection_graphics_mod_change = false; // From existing code
  std::array<float, 16> m_projection_matrix; // From existing code

  //alignas(16) std::array<float, 16> m_projection_matrix;

  // track changes
  //bool m_projection_graphics_mod_change = false;

  Common::Matrix44 LoadProjectionMatrix();
};
