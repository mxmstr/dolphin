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
#include "VideoCommon/VideoCommon.h" // For EFBRectangle (if used, or MathUtil::Rectangle)

class PointerWrap;
struct PortableVertexDeclaration;
class XFStateManager;

// From Hydra
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
// extern enum ViewportType g_viewport_type; // Now member
// extern enum ViewportType g_old_viewport_type; // Now member
// extern bool g_is_skybox; // Now member

// The non-API dependent parts.
class alignas(16) VertexShaderManager
{
public:
  ViewportType m_viewport_type = VIEW_FULLSCREEN;
  ViewportType m_old_viewport_type = VIEW_FULLSCREEN;
  bool m_is_skybox = false;

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

  VertexShaderConstants constants{};
  // From Hydra: (or equivalents will be needed)
  // static std::vector<VertexShaderConstants> constants_replay; // For opcode replay
  float4 eye_projection_left[4];  // Store left eye projection
  float4 eye_projection_right[4]; // Store right eye projection
  // float4 stereoparams_for_gs; // To be set for Geometry Shader

  bool dirty = false;

  // View manipulation members (from Hydra's static globals, now instance members)
  Matrix33 m_viewRotationMatrix;
  Matrix33 m_viewInvRotationMatrix;
  float m_viewTranslationVector[3];
  float m_viewRotation[2]; // Yaw, Pitch for free look
  Matrix44 m_gameProjectionMatrix;      // Game's original projection matrix
  Matrix44 m_viewportCorrectionMatrix;  // For viewport adjustments
  Matrix44 m_vrTotalMatrix;             // Final VR matrix sent to shader (can be one eye's)
  Matrix44 m_stabilizedGameCameraRot;
  float m_stabilizedGameCameraPos[3];
  bool m_had_skybox_locked = false;
  float m_locked_skybox_matrix[12]; // 3x4 matrix for skybox

  // VR state tracking
  bool m_layer_on_top = false; // from Hydra

  // Change tracking (simplified for now, will rely on XFStateManager mostly)
  // bool m_bTexMatricesChanged[2];
  // bool m_bPosNormalMatrixChanged;
  // bool m_bProjectionChanged;
  // bool m_bViewportChanged;


  // New methods from Hydra (adapted to be non-static for now)
  void SetProjectionConstants();
  void SetViewportConstants();
  void CheckOrientationConstants();
  void CheckSkybox();
  void LockSkybox();

  // void InvalidateXFRange(int start, int end); // XFStateManager handles this
  // void SetTexMatrixChangedA(u32 value); // XFStateManager
  // void SetTexMatrixChangedB(u32 value); // XFStateManager
  void SetViewportChanged(); // To signal internal changes
  void SetProjectionChanged(); // To signal internal changes
  // void SetMaterialColorChanged(int index); // XFStateManager

  void TranslateView(float left_metres, float forward_metres, float down_metres = 0.0f);
  void RotateView(float x, float y);
  void ScaleView(float scale);
  void ResetView();


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

  Common::Matrix44 LoadProjectionMatrix(); // This might become LoadStandardProjectionMatrix or be part of VR pipeline

  // VR related helpers
  void ClassifyCurrentDrawCall(XFStateManager& xf_state_manager);
  bool IsConsideredHUD(ViewportType type) const;
  Common::Matrix44 LoadGameProjectionMatrix(XFStateManager& xf_state_manager);

  void ApplySkyboxTransformations();
  void ApplyHUDTransformations();
  void ApplyWorldTransformations();

  Common::Matrix44 GetCurrentViewMatrix(); // Helper to get current game view matrix
  void CalculateStereoProjections(const Common::Matrix44& base_projection);
  void UpdateStereoParamsGS(); // To set stereoparams for geometry shader
};
