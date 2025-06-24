// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GeometryShaderManager.h"

#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

static constexpr int LINE_PT_TEX_OFFSETS[8] = {0, 16, 8, 4, 2, 1, 1, 1};

void GeometryShaderManager::Init()
{
  constants = {};

  // Init any intial constants which aren't zero when bpmem is zero.
  SetViewportChanged();
  SetProjectionChanged();

  dirty = true;
}

void GeometryShaderManager::Dirty()
{
  // This function is called after a savestate is loaded.
  // Any constants that can changed based on settings should be re-calculated
  m_projection_changed = true;

  // Uses EFB scale config
  SetLinePtWidthChanged();

  dirty = true;
}

void GeometryShaderManager::SetVSExpand(VSExpand expand)
{
  if (constants.vs_expand != expand)
  {
    constants.vs_expand = expand;
    dirty = true;
  }
}

void GeometryShaderManager::SetConstants(PrimitiveType prim)
{
  bool stereo_params_updated = false;
  if (m_projection_changed)
  {
    if (g_ActiveConfig.stereo_mode != StereoMode::Off &&
        g_ActiveConfig.stereo_mode != StereoMode::OpenVR)
    {
      // Calculate traditional stereo params (offset, convergence)
      if (xfmem.projection.type == ProjectionType::Perspective)
      {
        float offset = (g_ActiveConfig.iStereoDepth / 1000.0f) *
                       (g_ActiveConfig.iStereoDepthPercentage / 100.0f);
        constants.stereoparams[0] = g_ActiveConfig.bStereoSwapEyes ? offset : -offset;
        constants.stereoparams[1] = g_ActiveConfig.bStereoSwapEyes ? -offset : offset;
      }
      else
      {
        constants.stereoparams[0] = constants.stereoparams[1] = 0;
      }
      constants.stereoparams[2] = (float)(g_ActiveConfig.iStereoConvergence *
                                          (g_ActiveConfig.iStereoConvergencePercentage / 100.0f));
      constants.stereoparams[3] = 0.0f; // Ensure W is defined, though not used by current shader logic
      stereo_params_updated = true;
    }
    else if (g_ActiveConfig.stereo_mode == StereoMode::OpenVR)
    {
      // Zero out traditional stereo params as they are not used for projection in OpenVR mode.
      if (constants.stereoparams[0] != 0.0f || constants.stereoparams[1] != 0.0f ||
          constants.stereoparams[2] != 0.0f || constants.stereoparams[3] != 0.0f)
      {
        constants.stereoparams[0] = 0.0f;
        constants.stereoparams[1] = 0.0f;
        constants.stereoparams[2] = 0.0f;
        constants.stereoparams[3] = 0.0f;
        stereo_params_updated = true;
      }
    }
    else // StereoMode::Off or other unhandled cases related to m_projection_changed
    {
      // Ensure params are zero if stereo is off and projection changed
      if (constants.stereoparams[0] != 0.0f || constants.stereoparams[1] != 0.0f ||
          constants.stereoparams[2] != 0.0f || constants.stereoparams[3] != 0.0f)
      {
        constants.stereoparams[0] = 0.0f;
        constants.stereoparams[1] = 0.0f;
        constants.stereoparams[2] = 0.0f;
        constants.stereoparams[3] = 0.0f;
        stereo_params_updated = true;
      }
    }
    m_projection_changed = false; // Projection related updates are handled
  }

  if (stereo_params_updated)
  {
    dirty = true;
  }

  if (g_ActiveConfig.UseVSForLinePointExpand())
  {
    if (prim == PrimitiveType::Points)
      SetVSExpand(VSExpand::Point);
    else if (prim == PrimitiveType::Lines)
      SetVSExpand(VSExpand::Line);
    else
      SetVSExpand(VSExpand::None);
  }

  if (m_viewport_changed)
  {
    m_viewport_changed = false;

    constants.lineptparams[0] = 2.0f * xfmem.viewport.wd;
    constants.lineptparams[1] = -2.0f * xfmem.viewport.ht;

    dirty = true;
  }
}

void GeometryShaderManager::SetViewportChanged()
{
  m_viewport_changed = true;
}

void GeometryShaderManager::SetProjectionChanged()
{
  m_projection_changed = true;
}

void GeometryShaderManager::SetLinePtWidthChanged()
{
  constants.lineptparams[2] = bpmem.lineptwidth.linesize / 6.f;
  constants.lineptparams[3] = bpmem.lineptwidth.pointsize / 6.f;
  constants.texoffset[2] = LINE_PT_TEX_OFFSETS[bpmem.lineptwidth.lineoff];
  constants.texoffset[3] = LINE_PT_TEX_OFFSETS[bpmem.lineptwidth.pointoff];
  dirty = true;
}

void GeometryShaderManager::SetTexCoordChanged(u8 texmapid)
{
  TCoordInfo& tc = bpmem.texcoords[texmapid];
  int bitmask = 1 << texmapid;
  constants.texoffset[0] &= ~bitmask;
  constants.texoffset[0] |= tc.s.line_offset << texmapid;
  constants.texoffset[1] &= ~bitmask;
  constants.texoffset[1] |= tc.s.point_offset << texmapid;
  dirty = true;
}

void GeometryShaderManager::DoState(PointerWrap& p)
{
  p.Do(m_projection_changed);
  p.Do(m_viewport_changed);

  p.Do(constants);

  if (p.IsReadMode())
  {
    // Fixup the current state from global GPU state
    // NOTE: This requires that all GPU memory has been loaded already.
    Dirty();
  }
}
