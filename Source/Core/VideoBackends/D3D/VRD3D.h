// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#define OVR_D3D_VERSION 11

#include <windows.h>
#include "VideoCommon/VR.h"
//#include "VideoCommon/VR920.h"

#include "d3d11.h"

// OpenVR D3D support is usually handled by including openvr.h
// and then using vr::Texture_t with D3D11 texture pointers.
// No specific OpenVR D3D header is typically needed here beyond what's in openvr.h (included via VR.h).

namespace DX11
{
// Forward declare DXTexture
// class DXTexture;  // Not needed if m_frontBuffer is removed
// extern DXTexture* m_frontBuffer[2]; // Removed, m_frontBuffer is not the OpenVR eye texture array

void VR_ConfigureHMD();
void GetEyeTextureDimensions(int eye, UINT* width, UINT* height);
void VR_StartFramebuffer();
void VR_StopFramebuffer();
void VR_RenderToEyebuffer(int eye, int hmd_number = 0);
void VR_BeginFrame();
void VR_PresentHMDFrame();
void VR_DrawTimewarpFrame();
}
